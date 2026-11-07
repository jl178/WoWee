#pragma once

#include "game/world_packets.hpp"
#include "game/damage_meter.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/stock_combat_log.hpp"
#include "game/tab_target.hpp"
#include "network/packet.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
class Entity;

class CombatHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit CombatHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---
    void startAutoAttack(uint64_t targetGuid);
    void stopAutoAttack();
    [[nodiscard]] bool isAutoAttacking() const { return autoAttacking_; }
    [[nodiscard]] bool hasAutoAttackIntent() const { return autoAttackRequested_; }
    [[nodiscard]] bool isInCombat() const { return autoAttacking_ || !hostileAttackers_.empty(); }
    [[nodiscard]] bool isInCombatWith(uint64_t guid) const {
        return guid != 0 &&
               ((autoAttacking_ && autoAttackTarget_ == guid) ||
                (hostileAttackers_.count(guid) > 0));
    }
    [[nodiscard]] uint64_t getAutoAttackTargetGuid() const { return autoAttackTarget_; }
    [[nodiscard]] bool isAggressiveTowardPlayer(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    [[nodiscard]] uint64_t getLastMeleeSwingMs() const { return lastMeleeSwingMs_; }

    // Floating combat text
    [[nodiscard]] const std::vector<CombatTextEntry>& getCombatText() const { return combatText_; }
    void updateCombatText(float deltaTime);

    // Combat log (persistent rolling history)
    [[nodiscard]] const std::deque<CombatLogEntry>& getCombatLog() const { return combatLog_; }
    /// How many entries the log has ever been given. It holds the last 500, so
    /// this, not its size, says how many arrived since a reader last looked.
    [[nodiscard]] uint64_t getCombatLogAppended() const { return combatLogAppended_; }

    /// The group's damage meter, fed from the combat log below.
    [[nodiscard]] DamageMeter& damageMeter() { return damageMeter_; }
    /// The clock the meter's fights are timed on, in seconds.
    [[nodiscard]] static double meterClock();

    // Threat
    struct ThreatEntry {
        uint64_t victimGuid = 0;
        uint32_t threat     = 0;
    };
    [[nodiscard]] const std::vector<ThreatEntry>* getThreatList(uint64_t unitGuid) const {
        auto it = threatLists_.find(unitGuid);
        return (it != threatLists_.end()) ? &it->second : nullptr;
    }
    /// Every threat list the server has sent, by mob.
    [[nodiscard]] const std::unordered_map<uint64_t, std::vector<ThreatEntry>>& getThreatLists() const {
        return threatLists_;
    }

    // Hostile attacker tracking
    [[nodiscard]] bool isHostileAttacker(uint64_t guid) const { return hostileAttackers_.count(guid) > 0; }
    void clearHostileAttackers() { hostileAttackers_.clear(); }

    // Forced faction reactions
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getForcedReactions() const { return forcedReactions_; }

    // Auto-attack timing state (read by GameHandler::update for retry/resend logic)
    bool& autoAttackOutOfRangeRef() { return autoAttackOutOfRange_; }
    float& autoAttackOutOfRangeTimeRef() { return autoAttackOutOfRangeTime_; }
    float& autoAttackRangeWarnCooldownRef() { return autoAttackRangeWarnCooldown_; }
    float& autoAttackResendTimerRef() { return autoAttackResendTimer_; }
    bool& autoAttackRetryPendingRef() { return autoAttackRetryPending_; }

    // Combat text creation (used by other handlers, e.g. spell handler for periodic damage)
    void addCombatText(CombatTextEntry::Type type, int32_t amount, uint32_t spellId,
                       bool isPlayerSource, uint8_t powerType = 0,
                       uint64_t srcGuid = 0, uint64_t dstGuid = 0);

    // --- The stock combat log (game/stock_combat_log.hpp) ---
    /// Whether this expansion's interface reads the 3.3.5a combat log events
    /// built here. TBC keeps the older layout through addCombatText, 1.12 has
    /// none.
    [[nodiscard]] bool stockCombatLogActive() const;
    /// An entry from a packet: its units' flags are taken now, as 0x750400
    /// does, and it waits in order until both names are known (0x74F910).
    void logCombatEvent(const stock_combat_log::Record& record);
    /// Hand Lua every waiting entry whose names have arrived, oldest first;
    /// one that waits too long goes with what is known.
    void flushCombatLog();
    /// What the builders check of a spell (Spell.dbc attributes, effects).
    [[nodiscard]] stock_combat_log::SpellRules combatLogSpellRules(uint32_t spellId) const;
    /// 0x74DCB0 for a unit, from what this client knows of it.
    [[nodiscard]] uint32_t combatLogUnitFlags(uint64_t guid) const;
    /// UNIT_DIED / UNIT_DESTROYED / UNIT_DISSIPATES when a unit's health
    /// reaches zero (0x7489C0 -> 0x7561E0 -> 0x752ED0).
    void logUnitDeath(uint64_t guid);

    // Spellsteal dedup (used by aura update handler)
    bool shouldLogSpellstealAura(uint64_t casterGuid, uint64_t victimGuid, uint32_t spellId);

    // Called from GameHandler::update() each frame
    void updateAutoAttack(float deltaTime);

    // --- Targeting ---
    /// False for creature corpses that are neither lootable nor skinnable -
    /// those cannot be selected. Living units, players and non-units are always selectable.
    [[nodiscard]] bool isSelectableUnit(uint64_t guid) const;
    void setTarget(uint64_t guid);
    void clearTarget();
    [[nodiscard]] std::shared_ptr<Entity> getTarget() const;
    void setFocus(uint64_t guid);
    void clearFocus();
    [[nodiscard]] std::shared_ptr<Entity> getFocus() const;
    void setMouseoverGuid(uint64_t guid);
    void targetLastTarget();
    void targetEnemy(bool reverse);
    void targetFriend(bool reverse);

    /// The four narrower cycles WoW binds keys to, and which answered nothing.
    ///
    /// Same walk as the two above with a tighter test: only players, or only
    /// the people in your group. Bindings.xml gives each of them a key by
    /// default, so each was a key that did nothing at all.
    void targetNearestEnemyPlayer(bool reverse);
    void targetNearestFriendPlayer(bool reverse);
    void targetNearestPartyMember(bool reverse);
    void targetNearestRaidMember(bool reverse);

    /// Back to the last hostile, or the last friendly, target.
    ///
    /// TargetLastTarget remembers one target whatever it was; these remember one
    /// of each, which is what makes them useful - a healer's last friendly
    /// target survives half a fight's worth of tab-targeting.
    void targetLastEnemy();
    void targetLastFriend();
    void tabTarget(float playerX, float playerY, float playerZ);

    /// TargetNearest and its six siblings (0x524FC0): one list of candidates
    /// kept between presses, see tab_target.hpp. Selects and sends at once.
    void targetNearest(TargetNearestMode mode, bool reverse);
    /// CanAttack (0x729740/0x729A70) from the player's side, as TargetNearest
    /// and a right-click ask it.
    [[nodiscard]] bool canAttackUnit(uint64_t guid) const;
    /// Whether a unit's model was inside the camera's view on the last frame
    /// drawn (the stock test reads the map entity's culling flag, 0x77F0B0).
    /// Without one every unit counts as seen.
    void setUnitInViewProvider(std::function<bool(uint64_t)> provider) {
        unitInViewProvider_ = std::move(provider);
    }
    void assistTarget();
    /// AssistUnit(unit): select what that unit has selected (0x525EB0).
    /// `namedOtherUnit` is a unit asked for by a name other than "target".
    void assistUnit(uint64_t unitGuid, bool namedOtherUnit);

    // --- PvP ---
    void togglePvp();

    // --- Death / Resurrection ---
    void releaseSpirit();
    [[nodiscard]] bool canReclaimCorpse() const;
    [[nodiscard]] float getCorpseReclaimDelaySec() const;
    void reclaimCorpse();
    void useSelfRes();
    void activateSpiritHealer(uint64_t npcGuid);
    void acceptResurrect();
    void declineResurrect();

    // --- XP ---
    static uint32_t killXp(uint32_t playerLevel, uint32_t victimLevel);
    void handleXpGain(network::Packet& packet);

    // State management (for resets, entity cleanup)
    void resetAllCombatState();
    void removeHostileAttacker(uint64_t guid);
    void clearCombatText();
    void removeCombatTextForGuid(uint64_t guid);

private:
    // One remembered target of each kind, for targetLastEnemy/Friend. Written
    // by setTarget as the old target is let go, so they are the target before
    // this one rather than this one.
    uint64_t lastEnemyTargetGuid_ = 0;
    uint64_t lastFriendTargetGuid_ = 0;

    /// TargetNearest*'s list and cursor (0x524FC0), kept between presses.
    tab_target::Cycle targetNearestCycle_;
public:
    /// The macro runner's switches (0xBEAF44/0xBEAF4C).
    tab_target::MacroOnce& macroOnce() { return macroOnce_; }
private:
    tab_target::MacroOnce macroOnce_;
    std::function<bool(uint64_t)> unitInViewProvider_;

    [[nodiscard]] bool isGroupMemberGuid(uint64_t guid) const;

    // --- The damage meter ---
    /// The player or a group member: the units the meter keeps bars for.
    [[nodiscard]] bool isMeterCombatant(uint64_t guid) const;
    /// Who a unit's numbers are credited to: itself if it has a bar, the
    /// owner when it is a pet, totem or guardian of someone who does, else 0.
    [[nodiscard]] uint64_t meterCombatantOf(uint64_t guid) const;
    /// The server's combat flag on the player or anyone in the group.
    [[nodiscard]] bool groupInCombat() const;
    void feedDamageMeter(const stock_combat_log::Record& record);
    DamageMeter damageMeter_;
    float meterCombatPoll_ = 0.0f;
    bool meterGroupInCombat_ = false;

    struct PendingCombatLog {
        stock_combat_log::Record rec;
        stock_combat_log::Resolved res;
        std::chrono::steady_clock::time_point at;
    };
    std::deque<PendingCombatLog> combatLogPending_;
    /// The name an entry shows for a unit; `waiting` when it is not known yet
    /// (a query is then asked for, as the name cache's GetRecord does).
    std::string combatLogName(uint64_t guid, bool& waiting) const;

    /// What 0x518E40 asks of a unit, answered from what this client knows.
    [[nodiscard]] tab_target::UnitFacts targetNearestFacts(uint64_t guid, const Unit& unit) const;

    // --- Packet handlers ---
    void handleAttackStart(network::Packet& packet);
    void handleAttackStop(network::Packet& packet);
    void handleAttackerStateUpdate(network::Packet& packet);
    void handleSpellDamageLog(network::Packet& packet);
    void handleSpellHealLog(network::Packet& packet);
    void handleSetForcedReactions(network::Packet& packet);
    void handleHealthUpdate(network::Packet& packet);
    void handlePowerUpdate(network::Packet& packet);
    void handleUpdateComboPoints(network::Packet& packet);
    void handlePvpCredit(network::Packet& packet);
    void handleProcResist(network::Packet& packet);
    void handleSpellDamageShield(network::Packet& packet);
    void handleSpellOrDamageImmune(network::Packet& packet);
    void handleResistLog(network::Packet& packet);
    void handlePetTameFailure(network::Packet& packet);
    void handlePetActionFeedback(network::Packet& packet);
    void handlePetCastFailed(network::Packet& packet);
    void handlePetBroken(network::Packet& packet);
    void handlePetLearnedSpell(network::Packet& packet);
    void handlePetUnlearnedSpell(network::Packet& packet);
    void handlePetMode(network::Packet& packet);
    void handleResurrectFailed(network::Packet& packet);

    void autoTargetAttacker(uint64_t attackerGuid);

    GameHandler& owner_;

    // --- Combat state ---
    bool autoAttacking_ = false;
    bool autoAttackRequested_ = false;
    bool autoAttackRetryPending_ = false;
    uint64_t autoAttackTarget_ = 0;
    bool autoAttackOutOfRange_ = false;
    float autoAttackOutOfRangeTime_ = 0.0f;
    float autoAttackRangeWarnCooldown_ = 0.0f;
    float autoAttackResendTimer_ = 0.0f;
    std::unordered_set<uint64_t> hostileAttackers_;
    std::vector<CombatTextEntry> combatText_;
    static constexpr size_t MAX_COMBAT_LOG = 500;
    std::deque<CombatLogEntry> combatLog_;
    uint64_t combatLogAppended_ = 0;

    struct RecentSpellstealLogEntry {
        uint64_t casterGuid = 0;
        uint64_t victimGuid = 0;
        uint32_t spellId = 0;
        std::chrono::steady_clock::time_point timestamp{};
    };
    static constexpr size_t MAX_RECENT_SPELLSTEAL_LOGS = 32;
    std::deque<RecentSpellstealLogEntry> recentSpellstealLogs_;

    uint64_t lastMeleeSwingMs_ = 0;

    // unitGuid → sorted threat list (descending by threat value)
    std::unordered_map<uint64_t, std::vector<ThreatEntry>> threatLists_;
    /// Which mobs the player is currently tanking, so the pulled-aggro sound
    /// plays on becoming the tank rather than on every update that says they
    /// still are. See the threat update handler.
    std::unordered_set<uint64_t> playerTopThreatOn_;
    /// Which mobs the player, not tanking, is at or over the tank's threat on:
    /// the stretch Aggro_Enter_Warning_State marks the start of.
    std::unordered_set<uint64_t> playerOverTankOn_;
    /// Each mob's victim, as SMSG_HIGHEST_THREAT_UPDATE last named it.
    std::unordered_map<uint64_t, uint64_t> threatVictims_;
    /// Whether an aggro warning applies at all where the player is - the same
    /// question IsThreatWarningEnabled answers for the indicator, asked from
    /// C++ so the sound and the picture agree about when they are wanted.
    [[nodiscard]] bool threatWarningWanted() const;

    // Forced faction reactions
    std::unordered_map<uint32_t, uint8_t> forcedReactions_;
};

} // namespace game
} // namespace wowee
