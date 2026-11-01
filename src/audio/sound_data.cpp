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
namespace ets { constexpr uint32_t kFields = 5, kEmote = 1, kRace = 2, kSex = 3, kSound = 4; }
namespace etx { constexpr uint32_t kFields = 19, kEmote = 2; }   // EmotesText
namespace emo { constexpr uint32_t kFields = 7, kAnim = 2; }     // Emotes
namespace cmd { constexpr uint32_t kFields = 28, kSound = 13; }
namespace csd { constexpr uint32_t kFields = 38, kExertion = 1, kExertionCritical = 2, kInjury = 3,
                kInjuryCritical = 4, kDeath = 6, kStand = 8, kFootstep = 9, kJumpStart = 26,
                kJumpEnd = 27, kSubmerge = 35, kSubmerged = 36, kWingFlap = 11, kFidget0 = 14,
                kCustomAttack0 = 19, kImpactType = 25; }
namespace wss { constexpr uint32_t kFields = 4, kSwingType = 1, kCrit = 2, kSound = 3; }  // WeaponSwingSounds2
// WeaponImpactSounds: ID, WeaponSubClassID, ParrySoundType (1 metal weapon,
// 0 wood), ImpactSoundID[10], CritImpactSoundID[10].
namespace wis { constexpr uint32_t kFields = 23, kSubclass = 1, kParryType = 2, kImpact0 = 3, kCrit0 = 13; }
// FootstepTerrainLookup: ID, CreatureFootstepID, TerrainSoundID, SoundID, SoundIDSplash.
namespace ftl { constexpr uint32_t kFields = 5, kFootstep = 1, kTerrain = 2, kSound = 3, kSplash = 4; }
// ItemGroupSounds: ID, Sound[4] - pickup, put-down, and two the 3.3.5 rows leave unused.
namespace igs { constexpr uint32_t kFields = 5, kPickup = 1, kPutDown = 2; }
namespace idi { constexpr uint32_t kFields = 25, kGroupSound = 12; }  // ItemDisplayInfo

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
    emoteSounds_.clear();
    emoteTextSoundAnims_.clear();
    soundCache_.clear();
    creatureSoundCache_.clear();
    swingSounds_.clear();
    impactRows_.clear();
    footsteps_.clear();
    itemGroupSounds_.clear();
    loader_ = loadDbc;
    itemDisplayInfo_.reset();
    itemDisplayInfoTried_ = false;

    soundEntries_ =table(loadDbc, "SoundEntries.dbc", se::kFields);
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
    if (auto dbc = table(loadDbc, "EmotesTextSound.dbc", ets::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const uint64_t key = (static_cast<uint64_t>(dbc->getUInt32(i, ets::kEmote)) << 16) |
                                 ((dbc->getUInt32(i, ets::kRace) & 0xFF) << 8) | (dbc->getUInt32(i, ets::kSex) & 0xFF);
            emoteSounds_[key] = dbc->getUInt32(i, ets::kSound);
        }
    }
    // Animations voiced through EmotesTextSound (TRAIN -> EmoteTrain): the
    // M2 $CSD path skips them so the voice does not play twice.
    if (!emoteSounds_.empty()) {
        auto texts = table(loadDbc, "EmotesText.dbc", etx::kFields);
        auto emotes = table(loadDbc, "Emotes.dbc", emo::kFields);
        if (texts && emotes) {
            std::unordered_set<uint32_t> voicedTextEmotes;
            for (const auto& [key, sound] : emoteSounds_) {
                voicedTextEmotes.insert(static_cast<uint32_t>(key >> 16));
            }
            for (uint32_t i = 0; i < texts->getRecordCount(); ++i) {
                if (!voicedTextEmotes.count(texts->getUInt32(i, 0))) continue;
                const int32_t row = emotes->findRecordById(texts->getUInt32(i, etx::kEmote));
                if (row < 0) continue;
                const uint32_t anim = emotes->getUInt32(static_cast<uint32_t>(row), emo::kAnim);
                if (anim != 0) emoteTextSoundAnims_.insert(anim);
            }
        }
    }
    chrRaces_ =table(loadDbc, "ChrRaces.dbc", cr::kFields);
    displayInfo_ = table(loadDbc, "CreatureDisplayInfo.dbc", cdi::kFields);
    modelData_ = table(loadDbc, "CreatureModelData.dbc", cmd::kFields);
    creatureSounds_ = table(loadDbc, "CreatureSoundData.dbc", csd::kFields);

    if (auto dbc = table(loadDbc, "WeaponSwingSounds2.dbc", wss::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const uint32_t key = (dbc->getUInt32(i, wss::kSwingType) << 1) | (dbc->getUInt32(i, wss::kCrit) ? 1u : 0u);
            swingSounds_[key] = dbc->getUInt32(i, wss::kSound);
        }
    }
    if (auto dbc = table(loadDbc, "WeaponImpactSounds.dbc", wis::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            ImpactRow row;
            for (uint32_t s = 0; s < static_cast<uint32_t>(ImpactSlot::COUNT); ++s) {
                row.normal[s] = dbc->getUInt32(i, wis::kImpact0 + s);
                row.crit[s] = dbc->getUInt32(i, wis::kCrit0 + s);
            }
            const uint32_t key = (dbc->getUInt32(i, wis::kSubclass) << 1) | (dbc->getUInt32(i, wis::kParryType) ? 1u : 0u);
            impactRows_[key] = row;
        }
    }
    if (auto dbc = table(loadDbc, "FootstepTerrainLookup.dbc", ftl::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const uint32_t key = (dbc->getUInt32(i, ftl::kFootstep) << 8) | (dbc->getUInt32(i, ftl::kTerrain) & 0xFF);
            // The file repeats a few keys (footstep set 6, ground 1); the
            // first row is kept.
            footsteps_.emplace(key, std::make_pair(dbc->getUInt32(i, ftl::kSound), dbc->getUInt32(i, ftl::kSplash)));
        }
    }
    if (auto dbc = table(loadDbc, "ItemGroupSounds.dbc", igs::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            itemGroupSounds_[dbc->getUInt32(i, 0)] = {dbc->getUInt32(i, igs::kPickup), dbc->getUInt32(i, igs::kPutDown)};
        }
    }

    loaded_ = true;
    LOG_INFO("SoundData: ", soundEntries_->getRecordCount(), " sounds, ", zoneMusic_.size(), " music sets, ",
             ambience_.size(), " ambiences, ", areas_.size(), " areas, ", impactRows_.size(),
             " weapon impact rows, ", footsteps_.size(), " footstep rows, ", itemGroupSounds_.size(),
             " item sound groups");
    return true;
}

uint32_t SoundData::weaponSwingSound(uint32_t swingType, bool crit) const {
    auto it = swingSounds_.find((swingType << 1) | (crit ? 1u : 0u));
    if (it == swingSounds_.end() && crit) it = swingSounds_.find(swingType << 1);
    return it == swingSounds_.end() ? 0 : it->second;
}

uint32_t SoundData::weaponImpactSound(uint32_t weaponSubclass, bool metalWeapon, ImpactSlot slot, bool crit) const {
    const auto s = static_cast<uint32_t>(slot);
    if (s >= static_cast<uint32_t>(ImpactSlot::COUNT)) return 0;
    auto it = impactRows_.find((weaponSubclass << 1) | (metalWeapon ? 1u : 0u));
    if (it == impactRows_.end()) it = impactRows_.find((weaponSubclass << 1) | (metalWeapon ? 0u : 1u));
    if (it == impactRows_.end()) return 0;
    const uint32_t id = crit ? it->second.crit[s] : it->second.normal[s];
    return id != 0 ? id : it->second.normal[s];
}

uint32_t SoundData::footstepSound(uint32_t creatureFootstepId, uint32_t terrainSoundId, bool splash) const {
    auto it = footsteps_.find((creatureFootstepId << 8) | (terrainSoundId & 0xFF));
    // Ground the set has no row for: its dirt step (TerrainSoundID 1), then
    // the "none" row (0) a few sets carry.
    if (it == footsteps_.end()) it = footsteps_.find((creatureFootstepId << 8) | 1u);
    if (it == footsteps_.end()) it = footsteps_.find(creatureFootstepId << 8);
    if (it == footsteps_.end()) return 0;
    if (splash && it->second.second != 0) return it->second.second;
    return it->second.first;
}

uint32_t SoundData::itemFoleySound(uint32_t itemDisplayId, bool pickup) const {
    if (itemDisplayId == 0 || itemGroupSounds_.empty()) return 0;
    if (!itemDisplayInfoTried_) {
        itemDisplayInfoTried_ = true;
        if (loader_) itemDisplayInfo_ = table(loader_, "ItemDisplayInfo.dbc", idi::kFields);
    }
    if (!itemDisplayInfo_) return 0;
    const int32_t row = itemDisplayInfo_->findRecordById(itemDisplayId);
    if (row < 0) return 0;
    const uint32_t group = itemDisplayInfo_->getUInt32(static_cast<uint32_t>(row), idi::kGroupSound);
    auto it = itemGroupSounds_.find(group);
    if (it == itemGroupSounds_.end()) return 0;
    return pickup ? it->second.first : it->second.second;
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
    cs.wingFlap = creatureSounds_->getUInt32(r, csd::kWingFlap);
    for (uint32_t i = 0; i < 5; ++i) cs.fidget[i] = creatureSounds_->getUInt32(r, csd::kFidget0 + i);
    for (uint32_t i = 0; i < 4; ++i) cs.customAttack[i] = creatureSounds_->getUInt32(r, csd::kCustomAttack0 + i);
    cs.impactType = creatureSounds_->getUInt32(r, csd::kImpactType);
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

const SoundEntry* SoundData::emoteSound(uint32_t textEmoteId, uint32_t raceId, uint32_t sex) const {
    const uint64_t key = (static_cast<uint64_t>(textEmoteId) << 16) | ((raceId & 0xFF) << 8) | (sex & 0xFF);
    auto it = emoteSounds_.find(key);
    return it == emoteSounds_.end() ? nullptr : sound(it->second);
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
