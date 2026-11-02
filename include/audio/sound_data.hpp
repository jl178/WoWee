#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace pipeline { class DBCFile; }
namespace audio {

/// One SoundEntries.dbc row: the files a sound id names and how to play them.
struct SoundEntry {
    uint32_t id = 0;
    std::string name;
    std::vector<std::string> files;  ///< Archive paths, backslash separated.
    float volume = 1.0f;             ///< VolumeFloat, 0..1.
    uint32_t flags = 0;
    float minDistance = 0.0f;
    float distanceCutoff = 0.0f;
};

/// One SoundEmitters.dbc row: a looping sound placed in the world by hand -
/// a waterfall's roar, a forge, a river bend. 632 rows in 3.3.5, 334 of them
/// in Northrend.
struct MapSoundEmitter {
    uint32_t id = 0;
    float position[3] = {0.0f, 0.0f, 0.0f};  ///< Server/wire coordinates, as the DBC stores them.
    uint32_t soundId = 0;                    ///< SoundEntries.dbc
    std::string name;
};

/// One ZoneMusic.dbc row. Index 0 is day, 1 is night.
struct ZoneMusicSet {
    uint32_t id = 0;
    std::string name;
    uint32_t silenceMinMs[2] = {0, 0};
    uint32_t silenceMaxMs[2] = {0, 0};
    uint32_t soundId[2] = {0, 0};
};

/// One ZoneIntroMusicTable.dbc row: the piece played on arrival in a city.
struct ZoneIntro {
    uint32_t id = 0;
    uint32_t soundId = 0;
    uint32_t minDelayMinutes = 0;
};

/// The sound columns of one AreaTable.dbc row.
struct AreaSounds {
    uint32_t parent = 0;
    uint32_t ambienceId = 0;   ///< SoundAmbience.dbc
    uint32_t zoneMusicId = 0;  ///< ZoneMusic.dbc
    uint32_t introId = 0;      ///< ZoneIntroMusicTable.dbc
    int32_t reverb = 0;           ///< SoundProviderPreferences.dbc; 0 none
    int32_t reverbUnderwater = 0; ///< the same, with the listener submerged
    std::string name;
};

/// One WMOAreaTable.dbc row: what a building, or one room of it, sounds like.
struct WmoAreaSounds {
    uint32_t wmoId = 0;
    uint16_t nameSet = 0;
    int32_t groupId = -1;     ///< MOGP group id, not the file index; -1 covers the whole building
    uint32_t ambienceId = 0;  ///< SoundAmbience.dbc
    uint32_t zoneMusicId = 0; ///< ZoneMusic.dbc
    uint32_t introId = 0;
    uint32_t areaId = 0;      ///< AreaTable.dbc, 0 when the row has none
    int32_t reverb = 0;           ///< SoundProviderPreferences.dbc; 0 or -1 none
    int32_t reverbUnderwater = 0;
    std::string name;
};

/// One SoundProviderPreferences.dbc row: an EAX 2 room description, the
/// reverb the stock client asks its sound provider for in an area or room.
/// Levels are millibels (-10000 silent, 0 full), times are seconds.
struct SoundProviderPref {
    uint32_t id = 0;
    std::string name;
    float decayTime = 1.49f;
    float diffusion = 1.0f;
    int32_t room = -1000;          ///< master level of the room effect
    int32_t roomHF = -100;         ///< its level at high frequencies
    float decayHFRatio = 0.83f;    ///< how much faster the highs die away
    int32_t reflections = -2602;   ///< early reflections, against room
    float reflectionsDelay = 0.007f;
    int32_t reverb = 200;          ///< late reverb, against room
    float reverbDelay = 0.011f;
};

/// One NPCSounds.dbc row: what a creature says when spoken to.
struct NpcSounds {
    uint32_t hello = 0;
    uint32_t goodbye = 0;
    uint32_t pissed = 0;
    uint32_t ack = 0;
};

/// The sound ids one CreatureSoundData.dbc row gives a model.
struct CreatureSounds {
    uint32_t exertion = 0;
    uint32_t exertionCritical = 0;
    uint32_t injury = 0;
    uint32_t injuryCritical = 0;
    uint32_t death = 0;
    uint32_t stand = 0;
    uint32_t footstep = 0;       ///< FootstepTerrainLookup.CreatureFootstepID, not a sound
    uint32_t jumpStart = 0;
    uint32_t jumpEnd = 0;
    uint32_t submerge = 0;
    uint32_t submerged = 0;
    uint32_t wingFlap = 0;       ///< the model's $WNG events
    uint32_t fidget[5] = {};     ///< the model's $FD1..$FD5 events
    uint32_t customAttack[4] = {};  ///< the model's $AH0..$AH3 events
    uint32_t impactType = 0;     ///< CreatureImpactType: the victim column of WeaponImpactSounds
};

/// WeaponImpactSounds.dbc columns 3..12 (and the crit set 13..22): what a
/// weapon sounds like against each kind of victim, shield or parrying weapon.
enum class ImpactSlot : uint32_t {
    FLESH = 0, CHAIN = 1, PLATE = 2, SHIELD_METAL = 3, SHIELD_WOOD = 4,
    PARRY_METAL = 5, PARRY_WOOD = 6, WOOD = 7, STONE = 8, ETHEREAL = 9,
    COUNT = 10
};

/// The client's own sound tables, read once and queried by id.
///
/// The stock client picks zone music, zone ambience and a character's voice
/// from these tables. Wowee used to carry hand-written copies of the answers
/// and guess file names from race and gender; reading the tables gives the
/// same sounds the stock client plays, including the volumes it plays them
/// at, and works for areas and models the hand-written tables never listed.
///
/// Column indices are for build 3.3.5.12340 (WoWDBDefs). A table with fewer
/// columns than that layout is skipped rather than misread.
class SoundData {
public:
    using Loader = std::function<std::shared_ptr<pipeline::DBCFile>(const std::string& dbcName)>;

    /// Load every table through loadDbc. Returns false when SoundEntries.dbc
    /// is missing; every other table is optional. Tests feed synthetic tables.
    bool load(const Loader& loadDbc);
    [[nodiscard]] bool loaded() const { return loaded_; }

    [[nodiscard]] const SoundEntry* sound(uint32_t id) const;
    /// Lookup by SoundEntries.Name, case-insensitive.
    [[nodiscard]] const SoundEntry* soundByName(std::string_view name) const;
    [[nodiscard]] const ZoneMusicSet* zoneMusic(uint32_t id) const;
    [[nodiscard]] const ZoneIntro* intro(uint32_t id) const;
    [[nodiscard]] const AreaSounds* area(uint32_t areaId) const;

    // Area lookups follow ParentAreaID while the area's own column is 0, as
    // the stock client does: Northshire has no music of its own and plays
    // Elwynn's.
    [[nodiscard]] const ZoneMusicSet* areaMusic(uint32_t areaId) const;
    [[nodiscard]] uint32_t areaIntro(uint32_t areaId) const;
    [[nodiscard]] bool areaAmbience(uint32_t areaId, uint32_t& daySoundId, uint32_t& nightSoundId) const;
    /// Day and night loops for a SoundAmbience id.
    [[nodiscard]] bool ambience(uint32_t ambienceId, uint32_t& daySoundId, uint32_t& nightSoundId) const;

    /// The WMOAreaTable row for a room: the group's own row first, then the
    /// building's -1 row, each with the placement's name set and then set 0.
    /// groupId is the group's MOGP id, which the table is keyed on - not its
    /// index in the root file.
    [[nodiscard]] const WmoAreaSounds* wmoArea(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const;
    /// A room's SoundAmbience and ZoneMusic ids: the group's own row, then the
    /// building's -1 row when the group's names none. Ironforge's rooms name
    /// their own only here and there and the city's row covers the rest.
    [[nodiscard]] uint32_t wmoAmbience(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const;
    [[nodiscard]] uint32_t wmoZoneMusic(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const;

    /// A SoundProviderPreferences row by id; null for 0, -1 or an unknown id.
    [[nodiscard]] const SoundProviderPref* providerPref(int32_t id) const;
    /// SoundProviderPreferences.Description, case-sensitive ("PRESET_ROOM").
    [[nodiscard]] const SoundProviderPref* providerPrefByName(std::string_view name) const;
    /// The reverb an area asks for, following ParentAreaID while the area's
    /// own column is 0. 0 when the chain names none - most of the outdoors.
    [[nodiscard]] int32_t areaReverb(uint32_t areaId, bool underwater) const;
    /// The reverb a room asks for: the group's own row, then the building's
    /// -1 row when the group's names none. 0 when neither does.
    [[nodiscard]] int32_t wmoReverb(uint32_t wmoId, uint16_t nameSet, int32_t groupId, bool underwater) const;

    /// ChrRaces -> CreatureDisplayInfo -> CreatureModelData -> CreatureSoundData.
    [[nodiscard]] const CreatureSounds* playerSounds(uint32_t raceId, bool female) const;
    [[nodiscard]] const CreatureSounds* displaySounds(uint32_t displayId) const;
    /// CreatureDisplayInfo.NPCSoundID -> NPCSounds: the greeting, farewell
    /// and annoyed lines a display speaks. Null when the display has none.
    [[nodiscard]] const NpcSounds* npcSounds(uint32_t displayId) const;
    /// EmotesTextSound: the clip a text emote plays for a race and sex
    /// (sex 0 male, 1 female). Null when the emote is silent for them.
    [[nodiscard]] const SoundEntry* emoteSound(uint32_t textEmoteId, uint32_t raceId, uint32_t sex) const;
    /// True when some text emote that plays this animation (EmotesText ->
    /// Emotes.AnimID) has EmotesTextSound rows. Its voice comes from that
    /// table, so the model's $CSD event for the animation is not played too.
    [[nodiscard]] bool animHasEmoteTextSound(uint32_t animId) const {
        return emoteTextSoundAnims_.count(animId) != 0;
    }
    /// ChrRaces.ClientFileString, "Human" or "Scourge". Empty when unknown.
    [[nodiscard]] std::string raceClientName(uint32_t raceId) const;
    /// The jump and land grunts are not in CreatureSoundData for player
    /// models; SoundEntries names them "<Race><Sex>MainJump" and
    /// "<Race><Sex>MainLand", typos included.
    [[nodiscard]] const SoundEntry* playerJumpSound(uint32_t raceId, bool female) const;
    [[nodiscard]] const SoundEntry* playerLandSound(uint32_t raceId, bool female) const;

    /// WeaponSwingSounds2: the swoosh for a swing type (0 light, 1 medium,
    /// 2 heavy) and whether the swing is a crit. 0 when the table has none.
    [[nodiscard]] uint32_t weaponSwingSound(uint32_t swingType, bool crit) const;
    /// WeaponImpactSounds: the sound a weapon subclass (Item.dbc class 2
    /// subclass; 13 Fist is the unarmed row) makes against a victim slot.
    /// metalWeapon picks the row by the swinging weapon's material (the
    /// table's second column: 1 metal, 0 wood), falling back to the other row
    /// of the subclass. 0 when nothing matches.
    [[nodiscard]] uint32_t weaponImpactSound(uint32_t weaponSubclass, bool metalWeapon, ImpactSlot slot,
                                             bool crit) const;
    /// FootstepTerrainLookup: the step a creature footstep set
    /// (CreatureSoundData.SoundFootstepID) makes on a ground sound type
    /// (TerrainType.SoundID: 1 dirt ... 9 soggy; 0 when unknown). splash picks
    /// the SoundIDSplash column, for feet in shallow water.
    [[nodiscard]] uint32_t footstepSound(uint32_t creatureFootstepId, uint32_t terrainSoundId, bool splash) const;
    /// ItemDisplayInfo.GroupSoundIndex -> ItemGroupSounds: the pickup or
    /// put-down sound of an item display. 0 when the display has no group.
    [[nodiscard]] uint32_t itemFoleySound(uint32_t itemDisplayId, bool pickup) const;
    /// The same, as a playable row: null when the display has no group or the
    /// group's sound names no row with files. The stock client plays the
    /// pickup sound when a loot slot is clicked and the put-down sound when
    /// SMSG_ITEM_PUSH_RESULT puts an item in the player's bags.
    [[nodiscard]] const SoundEntry* itemFoleyEntry(uint32_t itemDisplayId, bool pickup) const;
    /// SoundEntries "LOOTWINDOWCOINSOUND" (LootCoinSmall.wav), the one coin
    /// sound the stock client plays: on every change of PLAYER_FIELD_COINAGE,
    /// on a rise of the honor or arena point fields, and when money goes onto
    /// or comes off the cursor. It has no amount threshold. Null when the row
    /// is missing or has no files.
    [[nodiscard]] const SoundEntry* coinSound() const;
    /// True when VocalUISounds.dbc was read. A character's spoken errors then
    /// come from it alone; a missing row means the stock client says nothing.
    [[nodiscard]] bool hasVocalUiSounds() const { return !vocalUiSounds_.empty(); }
    /// VocalUISounds: the line a race and sex speak for a vocal UI error
    /// (VocalUIEnum: 0 inventory full, 15 no mana, 46 out of range, ...).
    /// pissed picks the PissedSoundID columns. Null when the row is missing
    /// or its sound id names no SoundEntries row.
    [[nodiscard]] const SoundEntry* vocalUiSound(uint32_t vocalUiEnum, uint32_t raceId, bool female,
                                                 bool pissed) const;
    /// SoundEmitters.dbc rows placed on a map. Null when the map has none.
    [[nodiscard]] const std::vector<MapSoundEmitter>* mapEmitters(uint32_t mapId) const {
        auto it = mapEmitters_.find(mapId);
        return it == mapEmitters_.end() ? nullptr : &it->second;
    }

private:
    [[nodiscard]] const SoundEntry* playerVoiceByName(uint32_t raceId, bool female, const char* suffix) const;

    bool loaded_ = false;
    std::shared_ptr<pipeline::DBCFile> soundEntries_;
    std::shared_ptr<pipeline::DBCFile> chrRaces_;
    std::shared_ptr<pipeline::DBCFile> displayInfo_;
    std::shared_ptr<pipeline::DBCFile> modelData_;
    std::shared_ptr<pipeline::DBCFile> creatureSounds_;

    std::unordered_map<std::string, uint32_t> soundIdsByName_;
    std::unordered_map<uint32_t, ZoneMusicSet> zoneMusic_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ambience_;
    std::unordered_map<uint32_t, ZoneIntro> intros_;
    std::unordered_map<uint32_t, AreaSounds> areas_;
    std::unordered_map<uint32_t, std::vector<WmoAreaSounds>> wmoAreas_;  // by wmoId
    std::unordered_map<uint32_t, SoundProviderPref> providerPrefs_;      // by id
    std::unordered_map<uint32_t, NpcSounds> npcSounds_;  // by NPCSounds id
    std::unordered_map<uint64_t, uint32_t> emoteSounds_;  // (emote<<16 | race<<8 | sex) -> sound
    std::unordered_set<uint32_t> emoteTextSoundAnims_;    // anim ids voiced by EmotesTextSound
    std::unordered_map<uint32_t, std::vector<MapSoundEmitter>> mapEmitters_;  // SoundEmitters, by map

    // WeaponSwingSounds2: (swingType << 1 | crit) -> sound.
    std::unordered_map<uint32_t, uint32_t> swingSounds_;
    // WeaponImpactSounds: (subclass << 1 | metal) -> normal[10], crit[10].
    struct ImpactRow {
        uint32_t normal[static_cast<uint32_t>(ImpactSlot::COUNT)] = {};
        uint32_t crit[static_cast<uint32_t>(ImpactSlot::COUNT)] = {};
    };
    std::unordered_map<uint32_t, ImpactRow> impactRows_;
    // FootstepTerrainLookup: (footstepId << 8 | terrainSoundId) -> sound, splash.
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> footsteps_;
    // ItemGroupSounds: group -> pickup, put-down.
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> itemGroupSounds_;
    // VocalUISounds: (enum << 8 | race) -> sound ids [normal male, normal
    // female, pissed male, pissed female]; 0 for none.
    std::unordered_map<uint32_t, std::array<uint32_t, 4>> vocalUiSounds_;
    // ItemDisplayInfo is large and only the item foley wants it: read on the
    // first ask, through the loader load() was given.
    Loader loader_;
    mutable std::shared_ptr<pipeline::DBCFile> itemDisplayInfo_;
    mutable bool itemDisplayInfoTried_ = false;

    // SoundEntries has ten thousand rows and a handful are ever asked for.
    mutable std::unordered_map<uint32_t, SoundEntry> soundCache_;
    mutable std::unordered_map<uint32_t, CreatureSounds> creatureSoundCache_;
};

/// The SoundEntries name Wow.exe plays with a UI message, by the message's
/// GlobalStrings key: "ERR_DUEL_REQUESTED" -> "LEVELUP".
///
/// Wow.exe keeps one table of every UI message (0xac8af0: key, how it is
/// shown, sound name, vocal line, chat type) and plays the sound column by
/// name whenever it raises the message (0x5216f0). 40 of its 730 rows name a
/// sound; the rest are silent or speak a VocalUISounds line instead. Null for
/// a key that names no sound or is not in the table. Names are as the table
/// spells them ("FRIENDJOINGAME"); SoundEntries lookups ignore case.
[[nodiscard]] const char* uiMessageSoundName(std::string_view messageKey);

} // namespace audio
} // namespace wowee
