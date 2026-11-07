// IGameState - what a macro option ([combat], [@focus,harm], ...) asks the game.
//
// The questions are the ones the stock condition handlers ask
// (UIMacroOptions.cpp, Wow.exe 12340: the handler table at 0xAD1560, see
// docs/re/macros-bindings.md), so a provider answers each one the way the
// matching Lua API does and MacroEvaluator keeps the stock rules for the
// arguments. Unit questions take the unit as the macro wrote it after
// target= or @ - a token ("focus", "party2target") or a name - and an empty
// unit means the current target, as 0x60ABF0 resolves it.
#pragma once

#include <cstdint>
#include <string>

namespace wowee {
namespace ui {

class IGameState {
public:
    virtual ~IGameState() = default;

    // --- The unit conditions (exists, help, harm, party, raid, dead,
    //     unithasvehicleui) ---
    [[nodiscard]] virtual bool unitExists(const std::string& unit) const = 0;
    /// help: the player can assist the unit (UnitCanAssist("player", unit)).
    [[nodiscard]] virtual bool unitCanAssist(const std::string& unit) const = 0;
    /// harm: the player can attack the unit (UnitCanAttack("player", unit)).
    [[nodiscard]] virtual bool unitCanAttack(const std::string& unit) const = 0;
    [[nodiscard]] virtual bool unitInParty(const std::string& unit) const = 0;
    [[nodiscard]] virtual bool unitInRaid(const std::string& unit) const = 0;
    [[nodiscard]] virtual bool unitIsDead(const std::string& unit) const = 0;
    [[nodiscard]] virtual bool unitHasVehicleUI(const std::string& unit) const = 0;

    // --- The player's own state ---
    /// combat: the player, or the player's pet, is in combat (0x5EF710).
    [[nodiscard]] virtual bool inCombat() const = 0;
    [[nodiscard]] virtual bool isFlyableArea() const = 0;
    [[nodiscard]] virtual bool isOutdoors() const = 0;
    [[nodiscard]] virtual bool isSwimming() const = 0;
    [[nodiscard]] virtual bool isFlying() const = 0;
    [[nodiscard]] virtual bool isMounted() const = 0;
    [[nodiscard]] virtual bool isStealthed() const = 0;
    /// group with no argument, and group:party (GetNumPartyMembers() > 0).
    [[nodiscard]] virtual bool inParty() const = 0;
    /// group:raid (GetNumRaidMembers() > 0).
    [[nodiscard]] virtual bool inRaid() const = 0;
    /// vehicleui: the player's vehicle shows the vehicle interface.
    [[nodiscard]] virtual bool playerHasVehicleUI() const = 0;

    // --- Numbers the bar/stance/spec conditions compare against ---
    /// GetShapeshiftForm(): 0 for none, else the stance bar index.
    [[nodiscard]] virtual int shapeshiftForm() const = 0;
    /// GetActionBarPage(), 1-based.
    [[nodiscard]] virtual int actionBarPage() const = 0;
    /// GetBonusBarOffset().
    [[nodiscard]] virtual int bonusBarOffset() const = 0;
    /// GetActiveTalentGroup(), 1-based.
    [[nodiscard]] virtual int activeTalentGroup() const = 0;

    // --- Names the pet/equipped/channeling/cursor/button conditions compare ---
    [[nodiscard]] virtual bool hasPet() const = 0;
    [[nodiscard]] virtual std::string petName() const = 0;
    /// The pet's creature family ("Wolf", "Imp"), localized; "" when none.
    [[nodiscard]] virtual std::string petFamily() const = 0;
    /// equipped/worn: an equipped item is of this type - an inventory type
    /// token (INVTYPE_SHIELD) or its global string, an item class or
    /// subclass name ("Shields", "Daggers") - IsEquippedItemType.
    [[nodiscard]] virtual bool isEquippedItemType(const std::string& type) const = 0;
    /// The spell the player is channeling, "" when not channeling.
    [[nodiscard]] virtual std::string channelingSpell() const = 0;
    /// What the cursor holds as 0x5EF490 names it: "item", "money", "spell",
    /// "merchant", "macro", "guildbankmoney", "equipmentset"; "" for nothing
    /// (a pet action or a companion counts as nothing too).
    [[nodiscard]] virtual std::string cursorType() const = 0;
    /// The mouse button the running click came from ("LeftButton",
    /// "RightButton", ...), "" when no click is running. The stock rule then
    /// takes LeftButton (0x5EF0D0).
    [[nodiscard]] virtual std::string clickedButton() const = 0;

    /// An option name the table does not have. The stock client prints
    /// ERR_UNKNOWN_MACRO_OPTION_S once, when the bracket is first parsed.
    virtual void reportUnknownOption(const std::string& name) const { (void)name; }
};

} // namespace ui
} // namespace wowee
