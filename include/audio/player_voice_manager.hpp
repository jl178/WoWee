#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <random>
#include <chrono>

namespace wowee {
namespace pipeline { class AssetManager; }

namespace audio {

class SoundData;

// Player character error speech lines ("I can't do that", "Not enough mana", ...)
// Each value maps to a VocalUISounds.dbc VocalUIEnum (vocalUiEnum below). The
// hand-built Sound\Character\...\{Race}{Gender}_err_{token}NN.wav names are
// only used when that table is missing.
enum class PlayerErrorSpeech : uint8_t {
    NO_MANA,
    NO_RAGE,
    NO_ENERGY,
    OUT_OF_RANGE,
    GENERIC_NO_TARGET,
    INVALID_ATTACK_TARGET,
    SPELL_COOLDOWN,
    ABILITY_COOLDOWN,
    ITEM_COOLDOWN,
    POTION_COOLDOWN,
    NO_AMMO,
    AMMO_ONLY,
    CHEST_IN_USE,
    INVENTORY_FULL,
    BAG_FULL,
    ITEM_MAX_COUNT,
    NOT_A_BAG,
    CANT_USE_ITEM,
    ITEM_LOCKED,
    MUST_EQUIP_ITEM,
    NOT_EQUIPPABLE,
    CANT_EQUIP_LEVEL,
    CANT_EQUIP_SKILL,
    CANT_EQUIP_EVER,
    CANT_DROP_SOULBOUND,
    CANT_TRADE_SOULBOUND,
    NOT_ENOUGH_MONEY,
    CANT_LEARN_SPELL,
    LOOT_TOO_FAR,
    LOOT_DIDNT_KILL,
    CANT_LOOT,
    ALREADY_IN_GROUP,
    PARTY_FULL,
    GUILD_PERMISSIONS,
    CANT_CREATE_HERE,
};

/// The VocalUISounds.dbc VocalUIEnum the stock client speaks for each line.
/// The enum meanings come from the SoundEntries names of the human rows
/// (enum 46 -> "HumanMale_CantCastOutofRange"); each pick below names the
/// same _err_ files the old hand-built name did.
inline constexpr uint8_t kVocalUiEnum[] = {
    15,  // NO_MANA               CantCastNoMana
    63,  // NO_RAGE               NoRage
    64,  // NO_ENERGY             NoEnergy
    46,  // OUT_OF_RANGE          CantCastOutOfRange
    45,  // GENERIC_NO_TARGET     CantCastGenericNoTarget
    11,  // INVALID_ATTACK_TARGET InvalidTarget
    12,  // SPELL_COOLDOWN        SpellCooling
    50,  // ABILITY_COOLDOWN      CantUseAbilityCooling
    5,   // ITEM_COOLDOWN         CantUseItemCooling
    47,  // POTION_COOLDOWN       CantUsePotionCooling
    1,   // NO_AMMO               noammo
    28,  // AMMO_ONLY             CantPutOnlyAmmoInBag
    52,  // CHEST_IN_USE          CantUseChestInUse
    0,   // INVENTORY_FULL        InventoryFull
    29,  // BAG_FULL              CantPutBagFull
    30,  // ITEM_MAX_COUNT        CantPutItemMaxCount
    25,  // NOT_A_BAG             CantPutNotABag
    51,  // CANT_USE_ITEM         CantUseGeneric
    14,  // ITEM_LOCKED           Locked
    49,  // MUST_EQUIP_ITEM       CantUseMustEquipItem
    44,  // NOT_EQUIPPABLE        CantEquipNotEquippable
    2,   // CANT_EQUIP_LEVEL      NoEquipLevel
    41,  // CANT_EQUIP_SKILL      CantEquipNoSkill
    3,   // CANT_EQUIP_EVER       NoEquipEver
    4,   // CANT_DROP_SOULBOUND   SoulBoundNoDrop
    59,  // CANT_TRADE_SOULBOUND  CantTradeSoulbound
    40,  // NOT_ENOUGH_MONEY      CantAffordNotEnoughMoney
    13,  // CANT_LEARN_SPELL      CantLearnSpell
    35,  // LOOT_TOO_FAR          CantLootTooFar
    31,  // LOOT_DIDNT_KILL       CantLootDidntKill
    17,  // CANT_LOOT             CantLoot
    20,  // ALREADY_IN_GROUP      CantInviteAlreadyInGroup
    8,   // PARTY_FULL            CantInvite
    62,  // GUILD_PERMISSIONS     GuildPermissions
    18,  // CANT_CREATE_HERE      CantCreateHere
};
static_assert(sizeof(kVocalUiEnum) == static_cast<size_t>(PlayerErrorSpeech::CANT_CREATE_HERE) + 1,
              "one VocalUIEnum per PlayerErrorSpeech value");

[[nodiscard]] inline constexpr uint32_t vocalUiEnum(PlayerErrorSpeech type) {
    return kVocalUiEnum[static_cast<size_t>(type)];
}

/**
 * Plays the player character's spoken error responses ("I can't do that",
 * "Not enough mana", ...). The line comes from VocalUISounds.dbc and its
 * SoundEntries kit (files, volume) once setSoundData has been given loaded
 * tables; without them voice lines are found by file name and loaded lazily
 * for the active character's race/gender only. Gated by the "Character
 * Speech" setting.
 */
class PlayerVoiceManager {
public:
    PlayerVoiceManager() = default;
    ~PlayerVoiceManager() = default;

    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    void setEnabled(bool enabled) { enabled_ = enabled; }
    [[nodiscard]] bool isEnabled() const { return enabled_; }

    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    /// Play a spoken error response for the player's race/gender.
    /// raceId: game Race enum value (1=Human .. 11=Draenei); gender: 0=male, 1=female
    void playError(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender);

    /// The client's sound tables. While they hold VocalUISounds the hand-built
    /// file names are not used.
    void setSoundData(const SoundData* data) { soundData_ = data; }

private:
    struct SpeechSample {
        std::string path;
        std::vector<uint8_t> data;
    };

    // (Re)load the voice library when the active race/gender changes
    void ensureLibrary(uint8_t raceId, uint8_t gender);
    // Speak the VocalUISounds line; returns true when the line was played.
    bool playFromTable(PlayerErrorSpeech type, uint8_t raceId, uint8_t gender,
                       std::chrono::steady_clock::time_point now);

    pipeline::AssetManager* assetManager_ = nullptr;
    const SoundData* soundData_ = nullptr;
    bool enabled_ = true;
    float volumeScale_ = 1.0f;

    // Library for the currently loaded race/gender, keyed by PlayerErrorSpeech
    int loadedRace_ = -1;
    int loadedGender_ = -1;
    std::unordered_map<uint8_t, std::vector<SpeechSample>> library_;

    // Global throttle so key-mashing doesn't stack voice lines
    std::chrono::steady_clock::time_point lastPlayTime_{};
    static constexpr float PLAY_COOLDOWN = 1.0f;  // seconds

    // Repeats of one line, for the PissedSoundID columns. The stock rule is
    // not known; this is an approximation: the third time the same line is
    // spoken with no more than PISSED_WINDOW seconds between lines, the
    // pissed kit plays instead when the row has one. The 3.3.5 table names
    // pissed ids only in a few human rows, and none of them is a
    // SoundEntries row, so with stock data the normal line always plays.
    int repeatType_ = -1;
    int repeatCount_ = 0;
    std::chrono::steady_clock::time_point repeatTime_{};
    static constexpr float PISSED_WINDOW = 3.0f;  // seconds
    static constexpr int PISSED_REPEATS = 3;

    std::mt19937 rng_{std::random_device{}()};
};

} // namespace audio
} // namespace wowee
