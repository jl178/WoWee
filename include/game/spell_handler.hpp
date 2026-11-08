#pragma once

#include "game/stock_cast_timing.hpp"
#include "game/stock_cast_events.hpp"
#include "game/stock_trade_skill.hpp"

#include "game/world_packets.hpp"
#include "game/opcode_table.hpp"
#include "game/spell_defines.hpp"
#include "game/handler_types.hpp"
#include "audio/spell_sound_manager.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;

class SpellHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit SpellHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // Talent data structures (aliased from handler_types.hpp)
    using TalentEntry = game::TalentEntry;
    using TalentTabEntry = game::TalentTabEntry;

    // --- Spell book tabs ---
    struct SpellBookTab {
        std::string name;
        std::string texture; // icon path
        /// Every rank the book shows, which is the slot space GetSpellName,
        /// CastSpell and the rest index (ShowAllSpellRanks on).
        std::vector<uint32_t> spellIds;
        /// The same tab with each spell's lower ranks left out, as indices
        /// into spellIds. This is the highestRank* pair of GetSpellTabInfo and
        /// what GetKnownSlotFromHighestRankSlot maps back through - the view
        /// the book draws by default.
        std::vector<uint32_t> highestRankIdx;
    };

    // Unit cast state (aliased from handler_types.hpp)
    using UnitCastState = game::UnitCastState;

    // Equipment set info (aliased from handler_types.hpp)
    using EquipmentSetInfo = game::EquipmentSetInfo;

    // --- Public API (delegated from GameHandler) ---
    void castSpell(uint32_t spellId, uint64_t targetGuid = 0);

    // --- Spells cast at a point (spell targeting mode) ---
    // Pressing a spell whose Spell.dbc Targets waits on a location (Blizzard,
    // Flamestrike, Volley...) does not send it: the cursor carries it until
    // the ground is clicked (Wow.exe 0x80CCE0 / 0x80C790, docs/re/
    // spell-cast-pipeline.md section 15).
    struct GroundTargeting {
        uint32_t spellId = 0;
        /// The circle's half-width (0x8019C0), before the out-of-range case.
        float radius = 0.0f;
        float minRange = 0.0f;
        float maxRange = 0.0f;
        /// Set when an item's use is what waits (a bomb): the cast then goes
        /// out as CMSG_USE_ITEM for this bag slot.
        uint64_t itemGuid = 0;
        uint8_t itemBag = 0;
        uint8_t itemSlot = 0;
    };
    [[nodiscard]] bool isGroundTargeting() const { return groundTargeting_.spellId != 0; }
    [[nodiscard]] const GroundTargeting& groundTargeting() const { return groundTargeting_; }
    /// Put the spell down without casting it (SpellStopTargeting, right
    /// button, escape, another cast). True when one was waiting.
    bool stopGroundTargeting();
    /// An item whose use spell waits on a point: the use waits with it.
    /// False (and nothing changed) for any other spell.
    bool beginItemGroundTargeting(uint32_t spellId, uint8_t bag, uint8_t slot, uint64_t itemGuid);
    /// The point was clicked: cast there. canonical is the point on the
    /// ground in canonical coordinates; transportGuid the ship it lies on
    /// (then canonicalLocal is in the ship's frame), else 0.
    void castGroundTargetAt(const glm::vec3& canonical, uint64_t transportGuid,
                            const glm::vec3& canonicalLocal);

    /// Whether the spell flies a missile to its target (its visual has one
    /// and the spell a speed). Its target flinches when the missile lands,
    /// not when the damage is announced (Wow.exe 0x702DB0).
    bool spellFliesMissile(uint32_t spellId);

    /// Spell.dbc EffectImplicitTargetA, or 0 when the spell is unknown. 21 means
    /// the spell has to be aimed at a friendly unit.
    [[nodiscard]] uint32_t getSpellImplicitTargetA(uint32_t spellId) const;
    /// Whether Spell.dbc has this spell at all.
    ///
    /// A server may cast something the client's own data has never heard of -
    /// a private core's custom item does it routinely - and the answer to
    /// "what does this aim at" is then not zero but unknown. The two are worth
    /// telling apart: zero means the spell says nothing, unknown means we do.
    [[nodiscard]] bool isSpellKnownToClient(uint32_t spellId) const;

    /// The last spell the player cast while on foot. When mounting is detected,
    /// this identifies which of the player's indefinite self-cast auras is the
    /// mount - scanning for one blindly can land on a racial or a tracking buff.
    [[nodiscard]] uint32_t getLastGroundCastSpellId() const { return lastGroundCastSpellId_; }

    /// Record a spell cast by using an item - pre-WotLK mounts are items, and
    /// their on-use spell never passes through castSpell().
    void noteGroundCastSpell(uint32_t spellId) { lastGroundCastSpellId_ = spellId; }
    /// What starting to move does to the player's cast (0x5FAA40).
    void interruptForMoveStart();
    /// Whether a move start has anything to do here: a cast in flight, a
    /// channel, or the seated food and drink presentation.
    [[nodiscard]] bool moveStartHasWork() const {
        return casting_ || restorationActive_ || pendingCasts_.size() != 0;
    }
    void cancelCast();
    /// SpellStopCasting (0x809EA0): the current cast is put away (0x809A80 ->
    /// 0x806200 with SPELL_FAILED_DONT_REPORT), else a channel is asked to
    /// stop, else the auto-repeat is turned off. True when something stopped.
    bool spellStopCasting();
    /// The cast count of the cast the bar shows - the cast id
    /// UnitCastingInfo and the UNIT_SPELLCAST_* events carry.
    [[nodiscard]] uint8_t getCurrentCastCount() const { return currentCastCount_; }
    void cancelAura(uint32_t spellId);

    // Known spells
    [[nodiscard]] const std::unordered_set<uint32_t>& getKnownSpells() const { return knownSpells_; }
    [[nodiscard]] const std::unordered_map<uint32_t, float>& getSpellCooldowns() const { return spellCooldowns_; }
    [[nodiscard]] float getSpellCooldown(uint32_t spellId) const;
    [[nodiscard]] float getSpellCooldownTotal(uint32_t spellId) const;

    // Cast state
    [[nodiscard]] bool isCasting() const { return casting_ || restorationActive_; }
    [[nodiscard]] bool isChanneling() const { return casting_ ? castIsChannel_ : restorationActive_; }
    [[nodiscard]] bool isRestoring() const { return restorationActive_; }
    /// Auto Shot or Shoot while it repeats, otherwise 0.
    [[nodiscard]] uint32_t getAutoRepeatSpellId() const { return autoRepeatSpellId_; }
    void clearAutoRepeat() { autoRepeatSpellId_ = 0; }
    /// Turn a repeating Auto Shot or Shoot off: CMSG_CANCEL_AUTO_REPEAT_SPELL
    /// and the button's flash stops (0x807560(1)). Nothing when none runs.
    void cancelAutoRepeat();
    /// On its own cooldown, or held by the running global cooldown - what
    /// the stock client refuses before sending (0x739650 -> 0x807980).
    [[nodiscard]] bool isSpellNotReady(uint32_t spellId) const;
    [[nodiscard]] bool isGameObjectInteractionCasting() const;
    // The cast or channel in progress, else the ability queued for the next swing.
    [[nodiscard]] uint32_t getActiveActionSpellId() const {
        if (casting_) return currentCastSpellId_;
        return restorationActive_ ? restorationSpellId_ : nextSwingSpellId_;
    }
    [[nodiscard]] uint32_t getCurrentCastSpellId() const {
        return casting_ ? currentCastSpellId_ : restorationSpellId_;
    }
    /// The cast sent and not yet answered by SMSG_SPELL_START (0 otherwise).
    [[nodiscard]] uint32_t getPredictedCastSpellId() const { return predictedPrecastSpellId_; }
    [[nodiscard]] float getCastProgress() const {
        const float total = casting_ ? castTimeTotal_ : restorationTimeTotal_;
        const float remaining = casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
        return total > 0.0f ? (total - remaining) / total : 0.0f;
    }
    [[nodiscard]] float getCastTimeRemaining() const {
        return casting_ ? castTimeRemaining_ : restorationTimeRemaining_;
    }
    [[nodiscard]] float getCastTimeTotal() const {
        return casting_ ? castTimeTotal_ : restorationTimeTotal_;
    }

    // Repeat-craft queue: DoTradeSkill's run (0x5DAC00), kept in the trade
    // skill model as the stock client keeps it - asked for, then running from
    // the cast's start, one less at each cast's go.
    void startCraftQueue(uint32_t spellId, int count);
    void cancelCraftQueue();
    [[nodiscard]] int getCraftQueueRemaining() const {
        return tradeSkill_.repeatSpell ? tradeSkill_.repeatLeft
             : tradeSkill_.pendingSpell ? tradeSkill_.pendingCount : 0;
    }
    [[nodiscard]] uint32_t getCraftQueueSpellId() const {
        return static_cast<uint32_t>(tradeSkill_.repeatSpell ? tradeSkill_.repeatSpell
                                                             : tradeSkill_.pendingSpell);
    }

    // The trade skill window (Wow.exe TradeSkillFrame.cpp; see
    // stock_trade_skill.hpp). Opened by casting a spell whose first effect is
    // TRADE_SKILL (47), which sends nothing to the server, or by a "trade:"
    // link, which shows another player's list.
    [[nodiscard]] bool isCraftingWindowOpen() const { return tradeSkill_.isOpen(); }
    [[nodiscard]] uint32_t getCraftingSkillLine() const { return tradeSkill_.skillLine; }
    [[nodiscard]] const stock_trade_skill::Model& tradeSkill() const { return tradeSkill_; }
    [[nodiscard]] stock_trade_skill::Model& tradeSkillMutable() { return tradeSkill_; }
    /// 0x5DE120 for the player's own list: toggles closed when this line is
    /// already open, else (re)builds it and fires TRADE_SKILL_SHOW when it
    /// has rows.
    void openCraftingWindow(uint32_t openerSpell, uint32_t skillLine);
    /// The same for a "trade:" link (0x5DE300): someone else's recipes from
    /// the link's bits, at the link's rank. False when the link does not fit.
    bool openLinkedTradeSkill(const std::string& link);
    /// CloseTradeSkill (0x5DA8B0): stops a repeat and fires TRADE_SKILL_CLOSE.
    void closeCraftingWindow();
    /// 0x5DD5A0: the list made again (learned/unlearned recipe, a skill rank,
    /// the bags, an item reply it waited for). TRADE_SKILL_UPDATE when the
    /// list is complete.
    void rebuildTradeSkill();
    /// The rank the list is coloured by: the player's base skill, or a link's.
    [[nodiscard]] int tradeSkillRank() const;
    /// GetTradeSkillLine's three values (0x5DB810): with the permanent bonus
    /// for the player's own line.
    void tradeSkillLine(std::string& name, int& rank, int& maxRank) const;
    /// GetTradeSkillListLink (0x5DB060): the player's own list as a trade
    /// link, empty when the line cannot be linked.
    std::string tradeSkillListLink() const;
    /// What a recipe needs that is not a reagent, for GetTradeSkillTools
    /// (0x5DE620): name and whether it is there, in the stock order.
    std::vector<std::pair<std::string, bool>> tradeSkillTools(uint32_t spellId);
    /// Spell.dbc columns the trade skill window reads that the spell cache
    /// does not keep (WotLK layout).
    struct TradeSpellColumns {
        uint32_t effect[3] = {0, 0, 0};
        uint32_t itemType[3] = {0, 0, 0};
        int32_t basePoints[3] = {0, 0, 0};
        int32_t dieSides[3] = {0, 0, 0};
        int32_t equippedItemClass = -1;
        uint32_t equippedItemSubClassMask = 0;
        uint32_t equippedItemInvTypeMask = 0;
        uint32_t requiresSpellFocus = 0;
        uint32_t totem[2] = {0, 0};
        uint32_t totemCategory[2] = {0, 0};
        uint32_t areaGroup = 0;
        uint32_t attributes = 0;
    };
    const TradeSpellColumns* tradeSpellColumns(uint32_t spellId);
    /// Whether the player carries a tool of this TotemCategory (0x7548F0).
    bool playerHasTotemCategory(uint32_t totemCategoryId);
    // Returns the skill line id if spellId is a tradeskill-window opener
    // (e.g. Cooking → 185) with at least one known recipe, else 0.
    uint32_t tradeskillOpenerSkillLine(uint32_t spellId);

    // SpellFocusObject.dbc name ("Anvil", "Cooking Fire", ...) for
    // requires-spell-focus cast failures; empty if unknown.
    const std::string& getSpellFocusName(uint32_t focusId);

    // TotemCategory.dbc name ("Blacksmith Hammer", "Mining Pick", ...) for
    // totem-category cast failures; empty if unknown.
    const std::string& getTotemCategoryName(uint32_t categoryId);

    // Spell queue. Stock 3.3.5a has none; SpellQueueWindow (a later
    // client's CVar, ms, default 0 here) or the "Spell queue window"
    // enhancement (the reference client's 400 ms, docs/re/enhanced-combat.md) brings it.
    [[nodiscard]] uint32_t getQueuedSpellId() const {
        return queuedSpellId_ != 0 ? queuedSpellId_ : readyQueuedSpellId_;
    }
    void cancelQueuedSpell() {
        queuedSpellId_ = 0; queuedSpellTarget_ = 0;
        readyQueuedSpellId_ = 0; readyQueuedTarget_ = 0;
    }
    /// The enhancement's window in ms, or a negative value to read the
    /// SpellQueueWindow CVar (the stock default 0: no queue).
    void setSpellQueueOverrideMs(int ms) { spellQueueOverrideMs_ = ms; }
    /// The queue window in effect, ms; 0 is stock (no queue).
    [[nodiscard]] int spellQueueWindowMs() const;

    // Unit cast state (tracked per GUID for target frame + boss frames)
    [[nodiscard]] const UnitCastState* getUnitCastState(uint64_t guid) const {
        auto it = unitCastStates_.find(guid);
        return (it != unitCastStates_.end() && it->second.casting) ? &it->second : nullptr;
    }
    void clearUnitCastStates() { unitCastStates_.clear(); }
    void removeUnitCastState(uint64_t guid) { unitCastStates_.erase(guid); }

    // Aura cache mutation (formerly accessed via friend)
    void clearUnitAurasCache() { unitAurasCache_.clear(); }
    /// Copy a unit's auras into the by-guid list the party frames read.
    void mirrorAurasByGuid(uint64_t guid, const std::vector<AuraSlot>& auras);
    void removeUnitAuraCache(uint64_t guid) { unitAurasCache_.erase(guid); }

    // Known spells mutation (formerly accessed via friend)
    void addKnownSpell(uint32_t spellId) { knownSpells_.insert(spellId); }
    [[nodiscard]] bool hasKnownSpell(uint32_t spellId) const { return knownSpells_.count(spellId) > 0; }

    // Target aura mutation (formerly accessed via friend)
    /// The new target's auras as already known (every unit in range has its
    /// SMSG_AURA_UPDATEs cached), not an empty list: the server sends no
    /// AURA_UPDATE_ALL on a target change.
    void loadTargetAuras(uint64_t guid);

    // Player aura mutation (formerly accessed via friend)
    void resetPlayerAuras(size_t capacity) { playerAuras_.clear(); playerAuras_.resize(capacity); }
    AuraSlot& getPlayerAuraSlotRef(size_t slot) { return playerAuras_[slot]; }
    std::vector<AuraSlot>& getPlayerAurasMut() { return playerAuras_; }

    // Target cast helpers
    [[nodiscard]] bool isTargetCasting() const;
    [[nodiscard]] uint32_t getTargetCastSpellId() const;
    [[nodiscard]] float getTargetCastProgress() const;
    [[nodiscard]] float getTargetCastTimeRemaining() const;
    [[nodiscard]] bool isTargetCastInterruptible() const;

    // Talents
    [[nodiscard]] uint8_t getActiveTalentSpec() const { return activeTalentSpec_; }
    [[nodiscard]] uint8_t getNumTalentGroups() const { return talentGroupCount_; }
    [[nodiscard]] uint8_t getUnspentTalentPoints() const { return unspentTalentPoints_[activeTalentSpec_]; }
    [[nodiscard]] uint8_t getUnspentTalentPoints(uint8_t spec) const { return spec < 2 ? unspentTalentPoints_[spec] : 0; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents() const { return learnedTalents_[activeTalentSpec_]; }
    [[nodiscard]] const std::unordered_map<uint32_t, uint8_t>& getLearnedTalents(uint8_t spec) const {
        static std::unordered_map<uint32_t, uint8_t> empty;
        return spec < 2 ? learnedTalents_[spec] : empty;
    }

    static constexpr uint8_t MAX_GLYPH_SLOTS = 6;
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs() const { return learnedGlyphs_[activeTalentSpec_]; }
    [[nodiscard]] const std::array<uint16_t, MAX_GLYPH_SLOTS>& getGlyphs(uint8_t spec) const {
        static std::array<uint16_t, MAX_GLYPH_SLOTS> empty{};
        return spec < 2 ? learnedGlyphs_[spec] : empty;
    }
    [[nodiscard]] uint8_t getTalentRank(uint32_t talentId) const {
        auto it = learnedTalents_[activeTalentSpec_].find(talentId);
        return (it != learnedTalents_[activeTalentSpec_].end()) ? it->second : 0;
    }
    void learnTalent(uint32_t talentId, uint32_t requestedRank);
    /// (talentId, final rank from one), in the order the server should learn them.
    void learnPreviewTalents(const std::vector<std::pair<uint32_t, uint32_t>>& talents);
    void switchTalentSpec(uint8_t newSpec);

    // Talent DBC access
    [[nodiscard]] const TalentEntry* getTalentEntry(uint32_t talentId) const {
        auto it = talentCache_.find(talentId);
        return (it != talentCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const TalentTabEntry* getTalentTabEntry(uint32_t tabId) const {
        auto it = talentTabCache_.find(tabId);
        return (it != talentTabCache_.end()) ? &it->second : nullptr;
    }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentEntry>& getAllTalents() const { return talentCache_; }
    [[nodiscard]] const std::unordered_map<uint32_t, TalentTabEntry>& getAllTalentTabs() const { return talentTabCache_; }

    /// Drops what was read out of Talent.dbc and TalentTab.dbc.
    ///
    /// Called when the active expansion changes, for the same reason as
    /// MovementHandler::resetTaxiDbcCache: GameHandler cleared copies of its
    /// own and the readers come here.
    void resetTalentDbcCache() {
        talentCache_.clear();
        talentTabCache_.clear();
        talentDbcLoaded_ = false;
        // The spell book's own SkillLineAbility / SkillRaceClassInfo copies,
        // which are just as expansion-specific.
        spellBookSla_.clear();
        spellBookSrci_.clear();
        spellBookDbcLoaded_ = false;
        spellBookTabsDirty_ = true;
    }
    void loadTalentDbc();
    void syncPreWotlkTalentsFromKnownSpells();

    // Auras
    [[nodiscard]] const std::vector<AuraSlot>& getPlayerAuras() const { return playerAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>& getTargetAuras() const { return targetAuras_; }
    [[nodiscard]] const std::vector<AuraSlot>* getUnitAuras(uint64_t guid) const {
        auto it = unitAurasCache_.find(guid);
        return (it != unitAurasCache_.end()) ? &it->second : nullptr;
    }

    // Global Cooldown (GCD)
    [[nodiscard]] float getGCDRemaining() const {
        if (gcdTotal_ <= 0.0f) return 0.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gcdStartedAt_).count() / 1000.0f;
        float rem = gcdTotal_ - elapsed;
        return rem > 0.0f ? rem : 0.0f;
    }
    [[nodiscard]] float getGCDTotal() const { return gcdTotal_; }
    [[nodiscard]] bool isGCDActive() const { return getGCDRemaining() > 0.0f; }

    // Spell book tabs
    const std::vector<SpellBookTab>& getSpellBookTabs();

    // Talent wipe confirm dialog
    [[nodiscard]] bool showTalentWipeConfirmDialog() const { return talentWipePending_; }
    [[nodiscard]] uint32_t getTalentWipeCost() const { return talentWipeCost_; }
    /// The trainer offering the wipe. The confirmation closes itself when the
    /// player walks away from them.
    [[nodiscard]] uint64_t getTalentWipeNpcGuid() const { return talentWipeNpcGuid_; }
    void confirmTalentWipe();
    void cancelTalentWipe() { talentWipePending_ = false; }

    // Pet talent respec confirm
    void confirmPetUnlearn();
    void cancelPetUnlearn() { petUnlearnPending_ = false; }

    // Item use

    // Equipment sets - canonical data owned by InventoryHandler;
    // GameHandler::getEquipmentSets() delegates to inventoryHandler_.

    // Pet spells
    void sendPetAction(uint32_t action, uint64_t targetGuid = 0);
    void dismissPet();
    void togglePetSpellAutocast(uint32_t spellId);
    void renamePet(const std::string& newName);

    // Spell DBC accessors
    [[nodiscard]] const int32_t* getSpellEffectBasePoints(uint32_t spellId) const;
    [[nodiscard]] float getSpellDuration(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellName(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellRank(uint32_t spellId) const;
    [[nodiscard]] const std::string& getSpellDescription(uint32_t spellId) const;
    /// Spell.dbc ToolTip: what a buff or debuff's tooltip says it does.
    [[nodiscard]] const std::string& getSpellAuraDescription(uint32_t spellId) const;
    [[nodiscard]] std::string getEnchantName(uint32_t enchantId) const;
    /// The gem item an enchantment came out of (SpellItemEnchantment.Src_ItemID).
    /// Zero when the enchantment is not a gem, or on a file with no such column.
    /// This is the only route from an enchantment sitting in an item's socket
    /// back to the gem that is in the socket - the item fields carry the
    /// enchantment id and nothing else.
    [[nodiscard]] uint32_t getEnchantGemItem(uint32_t enchantId) const;
    /// SpellItemEnchantment.dbc Flags: bit 0 binds the item it is put on
    /// (AzerothCore ENCHANTMENT_CAN_SOULBOUND). The field just before Src_ItemID.
    [[nodiscard]] uint32_t getEnchantFlags(uint32_t enchantId) const;
    /// The SpellItemEnchantment word `offset` from Src_ItemID (see the .cpp).
    [[nodiscard]] uint32_t enchantWordNearGem(uint32_t enchantId, int offset) const;
    [[nodiscard]] uint8_t getSpellDispelType(uint32_t spellId) const;
    /// Whether the stock client lists this aura of `unitGuid` at all
    /// (stock_aura::listed): what UnitAura/UnitBuff count into.
    [[nodiscard]] bool isAuraListed(const AuraSlot& aura, uint64_t unitGuid) const;
    [[nodiscard]] bool isSpellInterruptible(uint32_t spellId) const;
    [[nodiscard]] bool isSpellPassive(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellSchoolMask(uint32_t spellId) const;
    /// Spell.dbc Targets mask (SpellCastTargetFlags): 0x10 = TARGET_FLAG_ITEM.
    [[nodiscard]] uint32_t getSpellTargetFlags(uint32_t spellId) const;
    [[nodiscard]] uint32_t getSpellTargetAuraState(uint32_t spellId) const;
    /// Stock's usable test past the power cost (0x80B5D0): combo points
    /// for a finishing move, the stance, and the caster's and target's aura
    /// states and aura spells. False greys an action button out.
    [[nodiscard]] bool spellRequirementsMet(uint32_t spellId) const;
    /// Spell.dbc RangeIndex resolved via SpellRange.dbc, in yards. Melee ("Combat
    /// Range") is 5; self-only is 0; negative means SpellRange.dbc was unavailable.
    [[nodiscard]] float getSpellMaxRange(uint32_t spellId) const;
    /// True for "Self Only" range spells (shouts, self-buffs): they always land on
    /// the caster, so they take no explicit target and skip melee range checks.
    [[nodiscard]] bool isSelfCastSpell(uint32_t spellId) const;
    /// Maps a superseded spell rank to the highest rank we actually know. Returns
    /// spellId unchanged when it is already known, or has no known same-name rank.
    [[nodiscard]] uint32_t resolveHighestKnownRank(uint32_t spellId) const;
    /// The skill's own name, by SkillLine.dbc id.
    [[nodiscard]] const std::string& getSkillLineName(uint32_t skillLineId) const;

    // Cast state
    void stopCasting();
    void resetCastState();
    void resetTalentState();
    // Full per-character reset (spells, cooldowns, auras, cast state, talents).
    // Called from GameHandler::selectCharacter so spell state doesn't bleed between characters.
    void resetAllState();
    void clearUnitCaches();

    // Aura duration
    void handleUpdateAuraDuration(uint8_t slot, uint32_t durationMs);

    // Skill DBC
    void loadSkillLineDbc();
    void extractSkillFields(const FlatFieldMap& fields);
    void extractExploredZoneFields(const FlatFieldMap& fields);

    // Update per-frame timers (call from GameHandler::update)
    void updateTimers(float dt);
    void refreshRestorationState() { refreshRestorationFromPlayerAuras(); }

    // Packet handlers dispatched from GameHandler's opcode table
    void handlePetSpells(network::Packet& packet);
    void handleListStabledPets(network::Packet& packet);

    // Pet stable commands (called via GameHandler delegation)
    void requestStabledPetList();
    void stablePet(uint8_t slot);
    void unstablePet(uint32_t petNumber);
    void swapStablePet(uint32_t petNumber);

    // DBC cache loading (called from GameHandler during login)
    void loadSpellNameCache() const;
    void loadSkillLineAbilityDbc();

    /// Ask the server what the pet is called.
    ///
    /// The name a player gave a pet arrives only in answer to
    /// CMSG_PET_NAME_QUERY. Without it a pet wears its creature template's
    /// name - "Voidwalker" where the player wrote something else - which is
    /// what the pet frame, its nameplate and the pet bar's tooltip all show.
    ///
    /// The pet number in that request is a key the server echoes back and does
    /// not look anything up with: SendPetNameQuery finds the pet by guid. So
    /// this sends a number of its own making and uses it to match the reply to
    /// the pet it asked about, which is what the field would have been for.
    void requestPetName(uint64_t petGuid);

    /// Give a hunter's pet up for good. The interface asks first - this is the
    /// other side of the ABANDON_PET dialog, whose accept called an unbound
    /// name and raised.
    /// The five values every UNIT_SPELLCAST_* event carries.
    ///
    /// unit, spell name, rank, cast id, spell id - in that order, which is what
    /// FrameXML unpacks. These were being fired as just the unit and the spell
    /// id, so the id sat where the name belongs and the cast id was absent.
    /// The cast id is the cast's count (0x60BF60 formats "%s%s%s%d" with the
    /// record's +0x24), so a report about one press is not read as one about
    /// another press of the same spell.
    [[nodiscard]] std::vector<std::string> spellcastArgs(const std::string& unitId,
                                           uint32_t spellId, uint8_t castCount) const;

    void abandonPet();

    /// Buy the next stable slot from the stable master currently open.
    void buyStableSlot();

    // One effect model on a unit's base, its $SND events included (see
    // SpellVisualSystem::playEffectModel). False when the unit is not known
    // or the model cannot be shown.
    bool playUnitEffectModel(uint64_t guid, const char* modelPath);

private:
    void announceLearnedSpell(uint32_t spellId);

    // --- Packet handlers ---
    void handleInitialSpells(network::Packet& packet);
    void handleCastFailed(network::Packet& packet);
    [[nodiscard]] bool answersPress(const CastFailedData& data, uint8_t castCount) const;
    void handleSpellStart(network::Packet& packet);
    void handleSpellGo(network::Packet& packet);
    /// COMBAT_TEXT_UPDATE for a spell that missed the player, once per miss.
    void announceSpellMissToCombatText(uint32_t spellId, uint64_t victimGuid,
                                       CombatTextEntry::Type ct);
    /// COMBAT_TEXT_UPDATE ENERGIZE / PERIODIC_ENERGIZE for the player's power.
    void announceEnergizeToCombatText(bool periodic, uint8_t powerType, uint32_t amount);
    uint32_t lastMissTextSpell_ = 0;
    const char* lastMissTextType_ = nullptr;
    std::chrono::steady_clock::time_point lastMissTextAt_{};
    void handleSpellCooldown(network::Packet& packet);
    void handleCooldownEvent(network::Packet& packet);
    void handleAuraUpdate(network::Packet& packet, bool isAll);
    void handleLearnedSpell(network::Packet& packet);

    void handleCastResult(network::Packet& packet);
    void handleSpellFailedOther(network::Packet& packet);
    void handleClearCooldown(network::Packet& packet);
    void handleModifyCooldown(network::Packet& packet);
    void handlePlaySpellVisual(network::Packet& packet);
    void handleSpellModifier(network::Packet& packet, bool isFlat);
    void handleSpellDelayed(network::Packet& packet);
    void handleSpellLogMiss(network::Packet& packet);
    void handleSpellFailure(network::Packet& packet);
    void handleItemCooldown(network::Packet& packet);
    void handleDispelFailed(network::Packet& packet);
    void handleTotemCreated(network::Packet& packet);
    void handlePeriodicAuraLog(network::Packet& packet);
    void handleSpellEnergizeLog(network::Packet& packet);
    void handleExtraAuraInfo(network::Packet& packet, bool isInit);
    void handleSpellDispelLog(network::Packet& packet);
    void handleSpellStealLog(network::Packet& packet);
    void handleSpellBreakLog(network::Packet& packet);
    void handleSpellChanceProcLog(network::Packet& packet);
    void handleSpellInstaKillLog(network::Packet& packet);
    void handleSpellLogExecute(network::Packet& packet);
    void handleClearExtraAuraInfo(network::Packet& packet);
    void handleItemEnchantTimeUpdate(network::Packet& packet);
    void handleResumeCastBar(network::Packet& packet);
    void handleChannelStart(network::Packet& packet);
    void handleChannelUpdate(network::Packet& packet);

    // --- Internal helpers ---

    // Resolve the magic school for a spell (for audio playback).
    // Returns MagicSchool from the spell name cache, defaulting to ARCANE.
    audio::SpellSoundManager::MagicSchool resolveSpellSchool(uint32_t spellId);

    // Play a spell cast or impact sound via audioCoordinator, if available.
    // Only for a spell none of whose SpellVisual kits names a sound (ANY):
    // the stock client plays a spell's kit sounds and nothing else - Flash
    // Heal's silent cast kit stays silent - so the school sound is a fallback
    // for spells that would otherwise make none at all.
    void playSpellCastSound(uint32_t spellId);
    void playSpellImpactSound(uint32_t spellId);
    enum class SpellSoundMoment : uint8_t { PRECAST, CAST, IMPACT, ANY };
    bool spellVisualPlaysSounds(uint32_t spellId, SpellSoundMoment moment);
    // The unit's auras' SpellVisual state kits, after its aura list changed.
    void syncAuraStateVisuals(uint64_t guid, const std::vector<AuraSlot>& auras);
    // Give the spell visual system the ways to find units: their model, feet
    // and channel target (once).
    void ensureSpellVisualResolvers();
    // Whom a unit channels at: UNIT_FIELD_CHANNEL_OBJECT, else whom it targets.
    uint64_t resolveChannelTarget(uint64_t casterGuid);
    // The radius of the area a spell lays (its largest effect radius,
    // SpellRadius.dbc), 0 when it names none.
    float resolveSpellAreaRadius(uint32_t spellId);
    // The area kits of a spell cast at a ground point (SPELL_GO dest).
    void triggerGroundAreaVisual(const SpellGoData& data, uint32_t visualId);

    // Resolve SpellVisualID from Spell.dbc cache for a given spellId.
    uint32_t resolveSpellVisualId(uint32_t spellId);
    // The visual this caster's cast of the spell plays: its own, merged with
    // the caster's ranged weapon's for a spell that uses the ranged slot
    // (Attributes 0x2: Auto Shot, Shoot, Throw - Wow.exe 0x720F80).
    uint32_t resolveCastVisualId(uint32_t spellId, uint64_t casterGuid);
    float resolveSpellMissileSpeed(uint32_t spellId);
    /// Fly the spell's missile from the caster to one target. True when one
    /// launched, which then plays the impact kit itself when it lands.
    /// castReleased: the caster plays the spell's release animation now (a
    /// cast or instant of its own, not a triggered spell), and the missile
    /// waits for that animation's release event before it leaves.
    bool launchSpellMissile(uint32_t spellId, uint32_t visualId, uint64_t casterGuid,
                            uint64_t targetGuid, bool impactAtEnd, bool castReleased = false,
                            uint32_t ammoDisplayId = 0, uint32_t ammoInventoryType = 0);
    // Resolve render-space position for a unit GUID (player or entity).
    bool resolveUnitPosition(uint64_t guid, glm::vec3& outPos);
    // Put the spell's build-up (PrecastKit, or ChannelKit for a channel) on
    // the caster until the cast resolves.
    void triggerCastVisual(uint32_t spellId, uint64_t casterGuid, uint32_t castTimeMs = 0,
                           bool channel = false);
    // Take a caster's build-up off (its cast went off, failed or ended).
    void cancelCastVisual(uint64_t casterGuid);
    // Play the impact visual effect at the target's position.
    // visualId: the visual to play (0: the spell's own).
    void triggerImpactVisual(uint32_t spellId, uint64_t targetGuid, bool targetIsCaster = false,
                             uint32_t visualId = 0);
    void refreshRestorationFromPlayerAuras();
    void stopRestorationPresentation();

    // --- handleSpellLogExecute per-effect parsers (extracted to reduce nesting) ---
    void parseEffectPowerDrain(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectHealthLeech(network::Packet& packet, uint32_t effectLogCount,
                                uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectCreateItem(network::Packet& packet, uint32_t effectLogCount,
                               uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectInterruptCast(network::Packet& packet, uint32_t effectLogCount,
                                  uint64_t caster, uint32_t spellId, bool isPlayerCaster);
    void parseEffectFeedPet(network::Packet& packet, uint32_t effectLogCount,
                            uint64_t caster, uint32_t spellId, bool isPlayerCaster);

    // Find the on-use spell for an item (trigger=0 Use or trigger=5 NoDelay).
    // CMSG_USE_ITEM requires a valid spellId or the server silently ignores it.
    void seedCooldownFromSpellInfo(uint32_t spellId);
    /// The cooldown an item's own template gives its spell, and its category's
    /// share of it (potions). See spell_handler.cpp.
    void seedItemSpellCooldown(uint64_t itemGuid, uint32_t spellId);
    /// Start a cooldown of `ms` on a spell now, and `categoryMs` on every
    /// carried item spell sharing `category`. Restarts one already running.
    void startItemCooldown(uint32_t spellId, uint32_t ms, uint32_t category, uint32_t categoryMs);
public:
    /// A cooldown that waits for SMSG_COOLDOWN_EVENT to begin - a potion
    /// drunk in combat, a spell with SPELL_ATTR0_DISABLED_WHILE_ACTIVE - in
    /// seconds, or 0 when none is pending. The item is unusable meanwhile and
    /// its square is greyed without a sweep (enable 0).
    [[nodiscard]] float getPendingEventCooldown(uint32_t spellId, uint32_t category = 0) const;
    /// The unit died: its cast pose and build-up are put away (the server's
    /// failure, usually first, ends the cast's record and bar).
    void onCasterDied(uint64_t guid);
private:
    void handleSupercededSpell(network::Packet& packet);
    void handleRemovedSpell(network::Packet& packet);
    void handleUnlearnSpells(network::Packet& packet);
    void handleTalentsInfo(network::Packet& packet);
    void handleAchievementEarned(network::Packet& packet);

    GameHandler& owner_;

    // --- Spell state ---
    std::unordered_set<uint32_t> knownSpells_;
    std::unordered_map<uint32_t, float> spellCooldowns_;    // spellId -> remaining seconds
    // spellId -> the length the cooldown had when it began. Kept beside the
    // remaining time rather than derived from it because GetSpellCooldown is
    // asked for (start, duration), and answering (now, remaining) redraws the
    // swirl as a fresh full sweep every time the interface asks - which it does
    // on every ACTIONBAR_UPDATE_COOLDOWN, so a long cooldown appears to restart
    // whenever anything else is cast.
    std::unordered_map<uint32_t, float> spellCooldownTotals_;
    /// Cooldowns started by SMSG_COOLDOWN_EVENT rather than by the cast:
    /// the spell's own length and its category's, held until the event.
    struct EventCooldown { uint32_t ms = 0; uint32_t category = 0; uint32_t categoryMs = 0; };
    std::unordered_map<uint32_t, EventCooldown> eventCooldowns_;
    uint8_t castCount_ = 0;
    GroundTargeting groundTargeting_;
    /// Whether the spell waits on a point, and if so the circle and range it
    /// is aimed with. False for anything else.
    bool groundTargetingFor(uint32_t spellId, GroundTargeting& out) const;
    bool casting_ = false;
    bool castIsChannel_ = false;
    uint32_t currentCastSpellId_ = 0;
    /// The cast count of the press the running cast answers, so a refusal of
    /// a later press during it is told apart from the cast's own failure.
    uint8_t currentCastCount_ = 0;
    /// The spell whose build-up castSpell already started on the send;
    /// SMSG_SPELL_START does not start it again.
    uint32_t predictedPrecastSpellId_ = 0;
    /// StartRecoveryCategory of the running global cooldown (133 nearly always).
    uint32_t gcdCategory_ = 133;
    /// 0x808200's once-per-burst rule for cast failure text and voice.
    stock_cast::CastErrorThrottle castErrorThrottle_;
    void startPredictedGcd(uint32_t spellId);
    void refuseCastLocally(uint32_t spellId, uint8_t result);
    /// The cast time after the player's SPELLMOD_CASTING_TIME mods (0x7FDB50).
    int32_t castTimeAfterMods(uint32_t spellId, int32_t baseMs);
    /// 0x739650's moving checks for the player's current movement flags.
    bool refusedForMoving(uint32_t spellId);
    /// Text and voice for a refused cast, through the throttle. False when
    /// the throttle held it back. castCount is the server's count for the
    /// refused press (-1 for the client's own refusals and for messages that
    /// carry none): a press already refused says nothing more.
    bool showCastFailure(uint32_t spellId, uint8_t result, uint32_t miscArg, uint32_t miscArg2,
                         bool fromServer, int castCount = -1);
    /// The refused press's count for the throttle: the wire's, where the
    /// message has one.
    [[nodiscard]] int throttleCastCount(uint8_t wireCount) const;
    [[nodiscard]] int playerPowerType() const;
    static int64_t steadyNowMs();
    uint32_t autoRepeatSpellId_ = 0;
    uint8_t autoRepeatCastCount_ = 0;
    /// The player's casts in flight (0xAF5254), from the send to the answer.
    stock_cast::PendingCasts pendingCasts_;
    /// The player's own cast as 0x7FECC0 compares it: the bar's cast.
    [[nodiscard]] stock_cast::UnitCast playerUnitCast() const;
    /// The count a server message names, or on the older clients (no count
    /// on the wire) the count of the record of that spell, if any.
    [[nodiscard]] uint8_t serverCastCount(uint8_t wireCount, uint32_t spellId) const;
    /// 0x806200: cancel one of the player's casts in flight - CMSG_CANCEL_CAST
    /// when it has a cast time, then the end of the cast with `result`.
    void cancelPendingCast(stock_cast::CastRef cast, uint8_t result);
    /// 0x7FECC0 for the player: end `cast` with `result` - the bar's cast
    /// state when it is that one, the event, the STOP - and forget its record.
    void endPlayerCast(stock_cast::CastRef cast, uint8_t result);
    /// The state the player's current cast leaves behind when it ends.
    void clearPlayerCastState();
    /// 0x7FECC0 for another unit, against its tracked cast.
    void endUnitCast(uint64_t guid, stock_cast::CastRef cast, uint8_t result);
    /// UNIT_SPELLCAST_<event> (and STOP) for every token naming the unit
    /// (0x60BF60 -> 0x60BB70).
    void fireCastEnd(uint64_t guid, const stock_cast::CastEndSteps& steps, stock_cast::CastRef cast);
    /// Resistances.dbc FizzleSoundID for the spell's school (0x4CFF30), at
    /// the player.
    /// The school's fizzle (Resistances.dbc FizzleSoundID). atUnit 0: as
    /// SMSG_CAST_FAILED plays it for the caster (0x808200); otherwise at that
    /// unit in the world with no own-character treatment, as SMSG_SPELL_FAILURE
    /// plays it for every caster (0x809c70 -> 0x4cff30).
    void playFizzle(uint32_t spellId, uint64_t atUnit = 0);
    // An on-next-swing ability between its SMSG_SPELL_START and the swing.
    uint32_t nextSwingSpellId_ = 0;
    uint8_t nextSwingCastCount_ = 0;
    // What the action buttons were last told, so a cast or swing changing is
    // announced once whichever of the many cast paths changed it.
    uint32_t announcedCastSpellId_ = 0;
    bool announcedAutoAttack_ = false;
    float castTimeRemaining_ = 0.0f;
    float castTimeTotal_ = 0.0f;
    bool restorationActive_ = false;
    uint32_t restorationSpellId_ = 0;
    bool restorationIsFood_ = false;
    float restorationTimeRemaining_ = 0.0f;
    float restorationTimeTotal_ = 0.0f;
    float restorationSoundTimer_ = 0.0f; // 0 until the aura's one fallback consume sound is due

    // The trade skill window and its repeat (stock_trade_skill.hpp).
    stock_trade_skill::Model tradeSkill_;
    std::vector<stock_trade_skill::Recipe> collectTradeSkillRecipes();
    void fireTradeSkillRecast();
    std::unordered_map<uint32_t, TradeSpellColumns> tradeSpellColumns_;
    bool tradeSpellColumnsLoaded_ = false;
    /// SkillLineAbility.dbc rows a trade link's bits are read against: the
    /// first unbroken run of a linkable line's rows in file order (0x812030,
    /// 0x8104A0).
    struct TradeAbilityRow {
        uint32_t spellId = 0;
        uint32_t raceMask = 0;
        uint32_t classMask = 0;
        int trivialHigh = 0;
        int trivialLow = 0;
    };
    std::unordered_map<uint32_t, std::vector<TradeAbilityRow>> tradeAbilityRuns_;
    bool tradeAbilityRunsLoaded_ = false;
    const std::vector<TradeAbilityRow>* tradeSkillAbilityRows(uint32_t skillLine);
    struct TradeSkillLineInfo {
        std::string name;
        std::string alternateVerb;
        bool canLink = false;
    };
    std::unordered_map<uint32_t, TradeSkillLineInfo> tradeSkillLines_;
    bool tradeSkillLinesLoaded_ = false;
public:
    const TradeSkillLineInfo* tradeSkillLineInfo(uint32_t skillLine);
    /// ItemSubClass.dbc VerboseName, else DisplayName (0x5DA6B0's header name).
    std::string itemSubClassHeaderName(int itemClass, int itemSubClass);
private:
    std::map<std::pair<int, int>, std::string> itemSubClassNames_;
    bool itemSubClassNamesLoaded_ = false;

    // SpellFocusObject.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> spellFocusNames_;
    bool spellFocusDbcLoaded_ = false;

    // TotemCategory.dbc names, loaded lazily
    std::unordered_map<uint32_t, std::string> totemCategoryNames_;
    bool totemCategoryDbcLoaded_ = false;

    // Spell queue (400ms window)
    uint32_t lastGroundCastSpellId_ = 0;
    /// When auto-attack was last toggled, for the Ability Toggle guard: a
    /// second press inside a short window is an accident rather than a
    /// decision. See the SPELL_ID_ATTACK branch in castSpell.
    std::chrono::steady_clock::time_point autoAttackToggledAt_{};
    uint32_t queuedSpellId_ = 0;
    uint64_t queuedSpellTarget_ = 0;
    /// A press refused only by the global cooldown or the spell's own
    /// cooldown, within the queue window of its end: sent by updateTimers the
    /// moment the spell is ready (Enhanced's SpellQueueWindow).
    uint32_t readyQueuedSpellId_ = 0;
    uint64_t readyQueuedTarget_ = 0;
    std::chrono::steady_clock::time_point readyQueuedAt_{};
    int spellQueueOverrideMs_ = -1;
    /// Seconds until `spellId` is no longer held by its cooldown or the GCD.
    [[nodiscard]] float notReadyWaitSec(uint32_t spellId) const;

    // Per-unit cast state
    std::unordered_map<uint64_t, UnitCastState> unitCastStates_;

    // Talents (dual-spec support)
    uint8_t activeTalentSpec_ = 0;
    // How many specs the character owns, as SMSG_TALENTS_INFO says: 1 until
    // Dual Talent Specialization is bought, then 2.
    uint8_t talentGroupCount_ = 1;
    uint8_t unspentTalentPoints_[2] = {0, 0};
    std::unordered_map<uint32_t, uint8_t> learnedTalents_[2];
    std::array<std::array<uint16_t, MAX_GLYPH_SLOTS>, 2> learnedGlyphs_{};
    std::unordered_map<uint32_t, TalentEntry> talentCache_;
    std::unordered_map<uint32_t, TalentTabEntry> talentTabCache_;
    bool talentDbcLoaded_ = false;
    bool talentsInitialized_ = false;

    // Auras
    std::vector<AuraSlot> playerAuras_;
    std::vector<AuraSlot> targetAuras_;
    std::unordered_map<uint64_t, std::vector<AuraSlot>> unitAurasCache_;

    // Global Cooldown
    float gcdTotal_ = 0.0f;
    std::chrono::steady_clock::time_point gcdStartedAt_{};
    // The cast that started the optimistic GCD, which a refused cast takes back.
    uint32_t optimisticGcdSpellId_ = 0;
    uint8_t optimisticGcdCastCount_ = 0;

    // Spell book tabs
    std::vector<SpellBookTab> spellBookTabs_;
    /// What the tabs were last built from: the known set (order-independent
    /// hash plus size), race, class and whether the DBCs were ready. A count
    /// alone missed a rank swap - SMSG_SUPERCEDED_SPELL removes one spell and
    /// adds another, and the book kept showing the old rank.
    uint64_t spellBookSignature_ = 0;
    bool spellBookTabsDirty_ = true;

    /// SkillLineAbility rows by spell, with the masks that say whose they are.
    /// The shared spellToSkillLine map keeps one line per spell, the last row
    /// read, which is a different class's row for a spell several classes
    /// share - so the book reads its own.
    struct SpellBookSlaRow {
        uint32_t skillLine = 0;
        uint32_t raceMask = 0;
        uint32_t classMask = 0;
        uint32_t excludeRace = 0;
        uint32_t excludeClass = 0;
    };
    std::unordered_map<uint32_t, std::vector<SpellBookSlaRow>> spellBookSla_;
    /// SkillRaceClassInfo rows by skill line: race mask, class mask, flags.
    struct SpellBookSrciRow { uint32_t raceMask = 0, classMask = 0, flags = 0; };
    std::unordered_map<uint32_t, std::vector<SpellBookSrciRow>> spellBookSrci_;
    bool spellBookDbcLoaded_ = false;
    void loadSpellBookDbc();

    // Talent wipe confirm dialog
    bool talentWipePending_ = false;
    uint64_t talentWipeNpcGuid_ = 0;
    uint32_t talentWipeCost_ = 0;

    // Pet talent respec confirm dialog
    bool petUnlearnPending_ = false;
    uint32_t petUnlearnCost_ = 0;
};

} // namespace game
} // namespace wowee
