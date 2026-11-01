#pragma once

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
    std::string name;
};

/// One WMOAreaTable.dbc row: what a building, or one room of it, sounds like.
struct WmoAreaSounds {
    uint32_t wmoId = 0;
    uint16_t nameSet = 0;
    int32_t groupIndex = -1;  ///< -1 covers the whole building
    uint32_t ambienceId = 0;  ///< SoundAmbience.dbc
    uint32_t zoneMusicId = 0; ///< ZoneMusic.dbc
    uint32_t introId = 0;
    uint32_t areaId = 0;      ///< AreaTable.dbc, 0 when the row has none
    std::string name;
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
    uint32_t footstep = 0;
    uint32_t jumpStart = 0;
    uint32_t jumpEnd = 0;
    uint32_t submerge = 0;
    uint32_t submerged = 0;
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
    [[nodiscard]] const WmoAreaSounds* wmoArea(uint32_t wmoId, uint16_t nameSet, int32_t groupIndex) const;

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
    std::unordered_map<uint32_t, NpcSounds> npcSounds_;  // by NPCSounds id
    std::unordered_map<uint64_t, uint32_t> emoteSounds_;  // (emote<<16 | race<<8 | sex) -> sound
    std::unordered_set<uint32_t> emoteTextSoundAnims_;    // anim ids voiced by EmotesTextSound

    // SoundEntries has ten thousand rows and a handful are ever asked for.
    mutable std::unordered_map<uint32_t, SoundEntry> soundCache_;
    mutable std::unordered_map<uint32_t, CreatureSounds> creatureSoundCache_;
};

} // namespace audio
} // namespace wowee
