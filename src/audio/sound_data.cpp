#include "audio/sound_data.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"

#include <cctype>

namespace wowee {
namespace audio {

namespace {

// Column indices for build 3.3.5.12340, from WoWDBDefs. kFields is the
// layout's full width; a table narrower than that is from another build.
namespace se  { constexpr uint32_t kFields = 30, kName = 2, kFile0 = 3, kFileCount = 10,
                kDirectory = 23, kVolume = 24, kFlags = 25, kMinDistance = 26, kDistanceCutoff = 27; }
namespace zm  { constexpr uint32_t kFields = 8, kSetName = 1, kSilenceMin = 2, kSilenceMax = 4, kSounds = 6; }
namespace sa  { constexpr uint32_t kFields = 3, kAmbience = 1; }
namespace at  { constexpr uint32_t kFields = 36, kParent = 2, kAmbience = 7, kZoneMusic = 8,
                kIntroSound = 9, kName = 11; }
namespace zi  { constexpr uint32_t kFields = 5, kSoundId = 2, kMinDelayMinutes = 4; }
namespace wa  { constexpr uint32_t kFields = 28, kWmoId = 1, kNameSet = 2, kGroup = 3, kAmbience = 6,
                kZoneMusic = 7, kIntroSound = 8, kAreaTable = 10, kName = 11; }
namespace cr  { constexpr uint32_t kFields = 69, kMaleDisplay = 4, kFemaleDisplay = 5, kClientFileString = 11; }
namespace cdi { constexpr uint32_t kFields = 16, kModel = 1, kSound = 2, kNpcSound = 12; }
namespace ns  { constexpr uint32_t kFields = 5, kSound0 = 1; }
namespace cmd { constexpr uint32_t kFields = 28, kSound = 13; }
namespace csd { constexpr uint32_t kFields = 38, kExertion = 1, kExertionCritical = 2, kInjury = 3,
                kInjuryCritical = 4, kDeath = 6, kStand = 8, kFootstep = 9, kJumpStart = 26,
                kJumpEnd = 27, kSubmerge = 35, kSubmerged = 36; }

constexpr int kMaxParentDepth = 16;

std::shared_ptr<pipeline::DBCFile> table(const SoundData::Loader& load, const char* name, uint32_t fields) {
    auto dbc = load(name);
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("SoundData: ", name, " not available");
        return nullptr;
    }
    if (dbc->getFieldCount() < fields) {
        LOG_WARNING("SoundData: ", name, " has ", dbc->getFieldCount(), " fields, the 3.3.5 layout has ",
                    fields, "; skipped");
        return nullptr;
    }
    return dbc;
}

std::string lowered(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool SoundData::load(const Loader& loadDbc) {
    loaded_ = false;
    soundIdsByName_.clear();
    zoneMusic_.clear();
    ambience_.clear();
    intros_.clear();
    areas_.clear();
    wmoAreas_.clear();
    npcSounds_.clear();
    soundCache_.clear();
    creatureSoundCache_.clear();

    soundEntries_ = table(loadDbc, "SoundEntries.dbc", se::kFields);
    if (!soundEntries_) return false;
    for (uint32_t i = 0; i < soundEntries_->getRecordCount(); ++i) {
        soundIdsByName_[lowered(soundEntries_->getStringView(i, se::kName))] = soundEntries_->getUInt32(i, 0);
    }

    if (auto dbc = table(loadDbc, "ZoneMusic.dbc", zm::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            ZoneMusicSet set;
            set.id = dbc->getUInt32(i, 0);
            set.name = dbc->getString(i, zm::kSetName);
            for (uint32_t d = 0; d < 2; ++d) {
                set.silenceMinMs[d] = dbc->getUInt32(i, zm::kSilenceMin + d);
                set.silenceMaxMs[d] = dbc->getUInt32(i, zm::kSilenceMax + d);
                set.soundId[d] = dbc->getUInt32(i, zm::kSounds + d);
            }
            zoneMusic_[set.id] = std::move(set);
        }
    }
    if (auto dbc = table(loadDbc, "SoundAmbience.dbc", sa::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            ambience_[dbc->getUInt32(i, 0)] = {dbc->getUInt32(i, sa::kAmbience), dbc->getUInt32(i, sa::kAmbience + 1)};
        }
    }
    if (auto dbc = table(loadDbc, "ZoneIntroMusicTable.dbc", zi::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            ZoneIntro intro;
            intro.id = dbc->getUInt32(i, 0);
            intro.soundId = dbc->getUInt32(i, zi::kSoundId);
            intro.minDelayMinutes = dbc->getUInt32(i, zi::kMinDelayMinutes);
            intros_[intro.id] = intro;
        }
    }
    if (auto dbc = table(loadDbc, "AreaTable.dbc", at::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            AreaSounds area;
            area.parent = dbc->getUInt32(i, at::kParent);
            area.ambienceId = dbc->getUInt32(i, at::kAmbience);
            area.zoneMusicId = dbc->getUInt32(i, at::kZoneMusic);
            area.introId = dbc->getUInt32(i, at::kIntroSound);
            area.name = dbc->getString(i, at::kName);
            areas_[dbc->getUInt32(i, 0)] = std::move(area);
        }
    }
    if (auto dbc = table(loadDbc, "WMOAreaTable.dbc", wa::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            WmoAreaSounds row;
            row.wmoId = dbc->getUInt32(i, wa::kWmoId);
            row.nameSet = static_cast<uint16_t>(dbc->getUInt32(i, wa::kNameSet));
            row.groupIndex = dbc->getInt32(i, wa::kGroup);
            row.ambienceId = dbc->getUInt32(i, wa::kAmbience);
            row.zoneMusicId = dbc->getUInt32(i, wa::kZoneMusic);
            row.introId = dbc->getUInt32(i, wa::kIntroSound);
            row.areaId = dbc->getUInt32(i, wa::kAreaTable);
            row.name = dbc->getString(i, wa::kName);
            wmoAreas_[row.wmoId].push_back(std::move(row));
        }
    }
    if (auto dbc = table(loadDbc, "NPCSounds.dbc", ns::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            NpcSounds row;
            row.hello = dbc->getUInt32(i, ns::kSound0);
            row.goodbye = dbc->getUInt32(i, ns::kSound0 + 1);
            row.pissed = dbc->getUInt32(i, ns::kSound0 + 2);
            row.ack = dbc->getUInt32(i, ns::kSound0 + 3);
            npcSounds_[dbc->getUInt32(i, 0)] = row;
        }
    }
    chrRaces_ = table(loadDbc, "ChrRaces.dbc", cr::kFields);
    displayInfo_ = table(loadDbc, "CreatureDisplayInfo.dbc", cdi::kFields);
    modelData_ = table(loadDbc, "CreatureModelData.dbc", cmd::kFields);
    creatureSounds_ = table(loadDbc, "CreatureSoundData.dbc", csd::kFields);

    loaded_ = true;
    LOG_INFO("SoundData: ", soundEntries_->getRecordCount(), " sounds, ", zoneMusic_.size(), " music sets, ",
             ambience_.size(), " ambiences, ", areas_.size(), " areas");
    return true;
}

const SoundEntry* SoundData::sound(uint32_t id) const {
    if (id == 0 || !soundEntries_) return nullptr;
    auto cached = soundCache_.find(id);
    if (cached != soundCache_.end()) return &cached->second;
    const int32_t row = soundEntries_->findRecordById(id);
    if (row < 0) return nullptr;
    const auto r = static_cast<uint32_t>(row);
    SoundEntry entry;
    entry.id = id;
    entry.name = soundEntries_->getString(r, se::kName);
    const std::string dir = soundEntries_->getString(r, se::kDirectory);
    for (uint32_t f = 0; f < se::kFileCount; ++f) {
        std::string file = soundEntries_->getString(r, se::kFile0 + f);
        if (file.empty()) continue;
        entry.files.push_back(dir.empty() ? std::move(file) : dir + "\\" + file);
    }
    entry.volume = soundEntries_->getFloat(r, se::kVolume);
    entry.flags = soundEntries_->getUInt32(r, se::kFlags);
    entry.minDistance = soundEntries_->getFloat(r, se::kMinDistance);
    entry.distanceCutoff = soundEntries_->getFloat(r, se::kDistanceCutoff);
    return &(soundCache_[id] = std::move(entry));
}

const SoundEntry* SoundData::soundByName(std::string_view name) const {
    auto it = soundIdsByName_.find(lowered(name));
    return it == soundIdsByName_.end() ? nullptr : sound(it->second);
}

const ZoneMusicSet* SoundData::zoneMusic(uint32_t id) const {
    auto it = zoneMusic_.find(id);
    return it == zoneMusic_.end() ? nullptr : &it->second;
}

const ZoneIntro* SoundData::intro(uint32_t id) const {
    auto it = intros_.find(id);
    return it == intros_.end() ? nullptr : &it->second;
}

const AreaSounds* SoundData::area(uint32_t areaId) const {
    auto it = areas_.find(areaId);
    return it == areas_.end() ? nullptr : &it->second;
}

const ZoneMusicSet* SoundData::areaMusic(uint32_t areaId) const {
    for (int depth = 0; areaId != 0 && depth < kMaxParentDepth; ++depth) {
        const AreaSounds* a = area(areaId);
        if (!a) return nullptr;
        if (a->zoneMusicId != 0) return zoneMusic(a->zoneMusicId);
        if (a->parent == areaId) return nullptr;
        areaId = a->parent;
    }
    return nullptr;
}

uint32_t SoundData::areaIntro(uint32_t areaId) const {
    for (int depth = 0; areaId != 0 && depth < kMaxParentDepth; ++depth) {
        const AreaSounds* a = area(areaId);
        if (!a) return 0;
        if (a->introId != 0) return a->introId;
        if (a->parent == areaId) return 0;
        areaId = a->parent;
    }
    return 0;
}

bool SoundData::areaAmbience(uint32_t areaId, uint32_t& daySoundId, uint32_t& nightSoundId) const {
    for (int depth = 0; areaId != 0 && depth < kMaxParentDepth; ++depth) {
        const AreaSounds* a = area(areaId);
        if (!a) return false;
        if (a->ambienceId != 0) {
            auto it = ambience_.find(a->ambienceId);
            if (it == ambience_.end()) return false;
            daySoundId = it->second.first;
            nightSoundId = it->second.second;
            return daySoundId != 0 || nightSoundId != 0;
        }
        if (a->parent == areaId) return false;
        areaId = a->parent;
    }
    return false;
}

bool SoundData::ambience(uint32_t ambienceId, uint32_t& daySoundId, uint32_t& nightSoundId) const {
    auto it = ambience_.find(ambienceId);
    if (it == ambience_.end()) return false;
    daySoundId = it->second.first;
    nightSoundId = it->second.second;
    return daySoundId != 0 || nightSoundId != 0;
}

const WmoAreaSounds* SoundData::wmoArea(uint32_t wmoId, uint16_t nameSet, int32_t groupIndex) const {
    auto it = wmoAreas_.find(wmoId);
    if (it == wmoAreas_.end()) return nullptr;
    const WmoAreaSounds* best = nullptr;
    int bestRank = 0;
    for (const WmoAreaSounds& row : it->second) {
        if (row.groupIndex != groupIndex && row.groupIndex != -1) continue;
        if (row.nameSet != nameSet && row.nameSet != 0) continue;
        const int rank = (row.groupIndex == groupIndex ? 2 : 0) + (row.nameSet == nameSet ? 1 : 0) + 1;
        if (rank > bestRank) {
            bestRank = rank;
            best = &row;
        }
    }
    return best;
}

const CreatureSounds* SoundData::displaySounds(uint32_t displayId) const {
    if (displayId == 0 || !displayInfo_ || !creatureSounds_) return nullptr;
    const int32_t displayRow = displayInfo_->findRecordById(displayId);
    if (displayRow < 0) return nullptr;
    uint32_t soundDataId = displayInfo_->getUInt32(static_cast<uint32_t>(displayRow), cdi::kSound);
    if (soundDataId == 0 && modelData_) {
        const uint32_t modelId = displayInfo_->getUInt32(static_cast<uint32_t>(displayRow), cdi::kModel);
        const int32_t modelRow = modelData_->findRecordById(modelId);
        if (modelRow >= 0) soundDataId = modelData_->getUInt32(static_cast<uint32_t>(modelRow), cmd::kSound);
    }
    if (soundDataId == 0) return nullptr;
    auto cached = creatureSoundCache_.find(soundDataId);
    if (cached != creatureSoundCache_.end()) return &cached->second;
    const int32_t row = creatureSounds_->findRecordById(soundDataId);
    if (row < 0) return nullptr;
    const auto r = static_cast<uint32_t>(row);
    CreatureSounds cs;
    cs.exertion = creatureSounds_->getUInt32(r, csd::kExertion);
    cs.exertionCritical = creatureSounds_->getUInt32(r, csd::kExertionCritical);
    cs.injury = creatureSounds_->getUInt32(r, csd::kInjury);
    cs.injuryCritical = creatureSounds_->getUInt32(r, csd::kInjuryCritical);
    cs.death = creatureSounds_->getUInt32(r, csd::kDeath);
    cs.stand = creatureSounds_->getUInt32(r, csd::kStand);
    cs.footstep = creatureSounds_->getUInt32(r, csd::kFootstep);
    cs.jumpStart = creatureSounds_->getUInt32(r, csd::kJumpStart);
    cs.jumpEnd = creatureSounds_->getUInt32(r, csd::kJumpEnd);
    cs.submerge = creatureSounds_->getUInt32(r, csd::kSubmerge);
    cs.submerged = creatureSounds_->getUInt32(r, csd::kSubmerged);
    return &(creatureSoundCache_[soundDataId] = cs);
}

const NpcSounds* SoundData::npcSounds(uint32_t displayId) const {
    if (displayId == 0 || !displayInfo_) return nullptr;
    const int32_t row = displayInfo_->findRecordById(displayId);
    if (row < 0) return nullptr;
    const uint32_t npcSoundId = displayInfo_->getUInt32(static_cast<uint32_t>(row), cdi::kNpcSound);
    auto it = npcSounds_.find(npcSoundId);
    return it == npcSounds_.end() ? nullptr : &it->second;
}

const CreatureSounds* SoundData::playerSounds(uint32_t raceId, bool female) const {
    if (!chrRaces_) return nullptr;
    const int32_t row = chrRaces_->findRecordById(raceId);
    if (row < 0) return nullptr;
    return displaySounds(chrRaces_->getUInt32(static_cast<uint32_t>(row), female ? cr::kFemaleDisplay : cr::kMaleDisplay));
}

std::string SoundData::raceClientName(uint32_t raceId) const {
    if (!chrRaces_) return {};
    const int32_t row = chrRaces_->findRecordById(raceId);
    if (row < 0) return {};
    return chrRaces_->getString(static_cast<uint32_t>(row), cr::kClientFileString);
}

const SoundEntry* SoundData::playerVoiceByName(uint32_t raceId, bool female, const char* suffix) const {
    const std::string race = raceClientName(raceId);
    if (race.empty()) return nullptr;
    // The data spells "Female" as "Feamle" in three of the twenty rows.
    static const char* const kFemale[] = {"Female", "Feamle", nullptr};
    static const char* const kMale[] = {"Male", nullptr};
    for (const char* const* sex = female ? kFemale : kMale; *sex; ++sex) {
        if (const SoundEntry* e = soundByName(race + *sex + suffix)) return e;
    }
    return nullptr;
}

const SoundEntry* SoundData::playerJumpSound(uint32_t raceId, bool female) const {
    return playerVoiceByName(raceId, female, "MainJump");
}

const SoundEntry* SoundData::playerLandSound(uint32_t raceId, bool female) const {
    return playerVoiceByName(raceId, female, "MainLand");
}

} // namespace audio
} // namespace wowee
