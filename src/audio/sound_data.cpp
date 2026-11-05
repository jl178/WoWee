#include "audio/sound_data.hpp"
#include "pipeline/dbc_loader.hpp"
#include "core/logger.hpp"
#include "game/wmo_area.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>

namespace wowee {
namespace audio {

namespace {

// Column indices for build 3.3.5.12340, from WoWDBDefs. kFields is the
// layout's full width; a table narrower than that is from another build.
namespace se  { constexpr uint32_t kFields = 30, kName = 2, kFile0 = 3, kFileCount = 10,
                kFreq0 = 13, kDirectory = 23, kVolume = 24, kFlags = 25, kMinDistance = 26,
                kDistanceCutoff = 27, kAdvanced = 29; }
namespace zm  { constexpr uint32_t kFields = 8, kSetName = 1, kSilenceMin = 2, kSilenceMax = 4, kSounds = 6; }
namespace sa  { constexpr uint32_t kFields = 3, kAmbience = 1; }
namespace at  { constexpr uint32_t kFields = 36, kParent = 2, kReverb = 5, kReverbUnderwater = 6,
                kAmbience = 7, kZoneMusic = 8, kIntroSound = 9, kName = 11; }
namespace zi  { constexpr uint32_t kFields = 5, kSoundId = 2, kMinDelayMinutes = 4; }
namespace wa  { constexpr uint32_t kFields = 28, kWmoId = 1, kNameSet = 2, kGroup = 3, kReverb = 4,
                kReverbUnderwater = 5, kAmbience = 6, kZoneMusic = 7, kIntroSound = 8, kAreaTable = 10,
                kName = 11; }
// SoundProviderPreferences: ID, Description, Flags, EAXEnvironmentSelection,
// EAXDecayTime, EAX2EnvironmentSize, EAX2EnvironmentDiffusion, EAX2Room,
// EAX2RoomHF, EAX2DecayHFRatio, EAX2Reflections, EAX2ReflectionsDelay,
// EAX2Reverb, EAX2ReverbDelay, then room rolloff, air absorption and the
// EAX 3 columns this client does not use.
namespace spp { constexpr uint32_t kFields = 24, kName = 1, kDecayTime = 4, kDiffusion = 6, kRoom = 7,
                kRoomHF = 8, kDecayHFRatio = 9, kReflections = 10, kReflectionsDelay = 11, kReverb = 12,
                kReverbDelay = 13; }
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
                kCustomAttack0 = 19, kImpactType = 25, kAggro = 10, kPetAttack = 28,
                kPetOrder = 29, kPetDismiss = 30; }
namespace wss { constexpr uint32_t kFields = 4, kSwingType = 1, kCrit = 2, kSound = 3; }  // WeaponSwingSounds2
// WeaponImpactSounds: ID, WeaponSubClassID, ParrySoundType (1 metal weapon,
// 0 wood), ImpactSoundID[10], CritImpactSoundID[10].
namespace wis { constexpr uint32_t kFields = 23, kSubclass = 1, kParryType = 2, kImpact0 = 3, kCrit0 = 13; }
// FootstepTerrainLookup: ID, CreatureFootstepID, TerrainSoundID, SoundID, SoundIDSplash.
namespace ftl { constexpr uint32_t kFields = 5, kFootstep = 1, kTerrain = 2, kSound = 3, kSplash = 4; }
// ItemGroupSounds: ID, Sound[4] - pickup, put-down, and two the 3.3.5 rows leave unused.
namespace igs { constexpr uint32_t kFields = 5, kPickup = 1, kPutDown = 2; }
namespace idi { constexpr uint32_t kFields = 25, kGroupSound = 12; }  // ItemDisplayInfo
// Weather: ID, AmbienceID (a SoundEntries row), EffectType, TransitionSkyBox,
// EffectColor[3], EffectTexture. SMSG_WEATHER's state is the row id.
namespace wth { constexpr uint32_t kFields = 8, kAmbience = 1; }
// SoundEmitters: ID, Position[3], Direction[3], SoundEntriesID, MapID, Name.
namespace sem { constexpr uint32_t kFields = 10, kPos = 1, kSound = 7, kMap = 8, kName = 9; }
// VocalUISounds: ID, VocalUIEnum, RaceID, NormalSoundID[2] (male, female),
// PissedSoundID[2]. An unused sound column holds 0 or 0xFFFFFFFF.
namespace vui { constexpr uint32_t kFields = 7, kEnum = 1, kRace = 2, kSound0 = 3; }
// SoundEntriesAdvanced: ID, SoundEntryID, InnerRadius2D, TimeA..D,
// RandomOffsetRange, Usage, TimeIntervalMin, TimeIntervalMax,
// VolumeSliderCategory, DuckToSFX, DuckToMusic, DuckToAmbience,
// InnerRadiusOfInfluence, OuterRadiusOfInfluence, TimeToDuck, TimeToUnduck,
// InsideAngle, OutsideAngle, OutsideVolume, OuterRadius2D, Name.
namespace sea { constexpr uint32_t kFields = 24, kInner2D = 2, kTimeA = 3, kOffset = 7, kUsage = 8,
                kIntervalMin = 9, kIntervalMax = 10, kCategory = 11, kDuck0 = 12, kInnerInfluence = 15,
                kOuterInfluence = 16, kTimeToDuck = 17, kTimeToUnduck = 18, kInsideAngle = 19,
                kOutsideAngle = 20, kOutsideVolume = 21, kOuter2D = 22; }

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
    providerPrefs_.clear();
    npcSounds_.clear();
    emoteSounds_.clear();
    emoteTextSoundAnims_.clear();
    soundCache_.clear();
    creatureSoundCache_.clear();
    swingSounds_.clear();
    impactRows_.clear();
    footsteps_.clear();
    itemGroupSounds_.clear();
    vocalUiSounds_.clear();
    advancedByKit_.clear();
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
            area.reverb = dbc->getInt32(i, at::kReverb);
            area.reverbUnderwater = dbc->getInt32(i, at::kReverbUnderwater);
            area.name = dbc->getString(i, at::kName);
            areas_[dbc->getUInt32(i, 0)] = std::move(area);
        }
    }
    if (auto dbc = table(loadDbc, "WMOAreaTable.dbc", wa::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            WmoAreaSounds row;
            row.wmoId = dbc->getUInt32(i, wa::kWmoId);
            row.nameSet = static_cast<uint16_t>(dbc->getUInt32(i, wa::kNameSet));
            row.groupId = dbc->getInt32(i, wa::kGroup);
            row.ambienceId = dbc->getUInt32(i, wa::kAmbience);
            row.zoneMusicId = dbc->getUInt32(i, wa::kZoneMusic);
            row.introId = dbc->getUInt32(i, wa::kIntroSound);
            row.areaId = dbc->getUInt32(i, wa::kAreaTable);
            row.reverb = dbc->getInt32(i, wa::kReverb);
            row.reverbUnderwater = dbc->getInt32(i, wa::kReverbUnderwater);
            row.name = dbc->getString(i, wa::kName);
            wmoAreas_[row.wmoId].push_back(std::move(row));
        }
    }
    if (auto dbc = table(loadDbc, "SoundProviderPreferences.dbc", spp::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            SoundProviderPref p;
            p.id = dbc->getUInt32(i, 0);
            p.name = dbc->getString(i, spp::kName);
            p.decayTime = dbc->getFloat(i, spp::kDecayTime);
            p.diffusion = dbc->getFloat(i, spp::kDiffusion);
            p.room = dbc->getInt32(i, spp::kRoom);
            p.roomHF = dbc->getInt32(i, spp::kRoomHF);
            p.decayHFRatio = dbc->getFloat(i, spp::kDecayHFRatio);
            p.reflections = dbc->getInt32(i, spp::kReflections);
            p.reflectionsDelay = dbc->getFloat(i, spp::kReflectionsDelay);
            p.reverb = dbc->getInt32(i, spp::kReverb);
            p.reverbDelay = dbc->getFloat(i, spp::kReverbDelay);
            providerPrefs_[p.id] = std::move(p);
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
    if (auto dbc = table(loadDbc, "Weather.dbc", wth::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            const uint32_t sound = dbc->getUInt32(i, wth::kAmbience);
            if (sound != 0 && sound != 0xFFFFFFFFu) weatherSounds_[dbc->getUInt32(i, 0)] = sound;
        }
    }
    if (auto dbc = table(loadDbc, "ItemGroupSounds.dbc", igs::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            itemGroupSounds_[dbc->getUInt32(i, 0)] = {dbc->getUInt32(i, igs::kPickup), dbc->getUInt32(i, igs::kPutDown)};
        }
    }
    if (auto dbc = table(loadDbc, "VocalUISounds.dbc", vui::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            std::array<uint32_t, 4> ids{};
            for (uint32_t c = 0; c < 4; ++c) {
                const uint32_t id = dbc->getUInt32(i, vui::kSound0 + c);
                ids[c] = id == 0xFFFFFFFFu ? 0 : id;
            }
            const uint32_t key = (dbc->getUInt32(i, vui::kEnum) << 8) | (dbc->getUInt32(i, vui::kRace) & 0xFF);
            vocalUiSounds_.emplace(key, ids);
        }
    }
    // The advanced rows, by the kit that names them. The kit's own column is
    // what the client follows (kit +0xa8, set by 0x4cd4a0), not the row's
    // SoundEntryID: two rows name a kit that does not name them back.
    if (auto dbc = table(loadDbc, "SoundEntriesAdvanced.dbc", sea::kFields)) {
        std::unordered_map<uint32_t, stock::AdvancedKit> byId;
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            stock::AdvancedKit k;
            k.id = dbc->getUInt32(i, 0);
            k.soundEntryId = dbc->getUInt32(i, 1);
            k.innerRadius2D = dbc->getFloat(i, sea::kInner2D);
            k.outerRadius2D = dbc->getFloat(i, sea::kOuter2D);
            for (uint32_t t = 0; t < 4; ++t) k.time[t] = dbc->getInt32(i, sea::kTimeA + t);
            k.randomOffsetRange = dbc->getInt32(i, sea::kOffset);
            k.usage = static_cast<stock::AdvancedUsage>(std::min<uint32_t>(dbc->getUInt32(i, sea::kUsage), 2));
            k.intervalMinMs = dbc->getInt32(i, sea::kIntervalMin);
            k.intervalMaxMs = dbc->getInt32(i, sea::kIntervalMax);
            const uint32_t cat = dbc->getUInt32(i, sea::kCategory);
            k.category = cat < static_cast<uint32_t>(stock::Category::Count) ? static_cast<stock::Category>(cat)
                                                                             : stock::Category::Sfx;
            for (uint32_t g = 0; g < 3; ++g) k.duck[g] = dbc->getFloat(i, sea::kDuck0 + g);
            k.innerRadiusOfInfluence = dbc->getFloat(i, sea::kInnerInfluence);
            k.outerRadiusOfInfluence = dbc->getFloat(i, sea::kOuterInfluence);
            k.timeToDuckMs = dbc->getInt32(i, sea::kTimeToDuck);
            k.timeToUnduckMs = dbc->getInt32(i, sea::kTimeToUnduck);
            k.insideAngle = dbc->getFloat(i, sea::kInsideAngle);
            k.outsideAngle = dbc->getFloat(i, sea::kOutsideAngle);
            k.outsideVolume = dbc->getFloat(i, sea::kOutsideVolume);
            byId[k.id] = stock::normaliseAdvancedKit(k);
        }
        for (uint32_t i = 0; i < soundEntries_->getRecordCount(); ++i) {
            const uint32_t adv = soundEntries_->getUInt32(i, se::kAdvanced);
            if (adv == 0) continue;
            if (auto it = byId.find(adv); it != byId.end())
                advancedByKit_[soundEntries_->getUInt32(i, 0)] = it->second;
        }
    }
    mapEmitters_.clear();
    size_t emitterCount = 0;
    if (auto dbc = table(loadDbc, "SoundEmitters.dbc", sem::kFields)) {
        for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
            MapSoundEmitter e;
            e.id = dbc->getUInt32(i, 0);
            for (uint32_t c = 0; c < 3; ++c) e.position[c] = dbc->getFloat(i, sem::kPos + c);
            e.soundId = dbc->getUInt32(i, sem::kSound);
            e.name = dbc->getString(i, sem::kName);
            if (e.soundId == 0) continue;
            mapEmitters_[dbc->getUInt32(i, sem::kMap)].push_back(std::move(e));
            ++emitterCount;
        }
    }

    loaded_ = true;
    LOG_INFO("SoundData: ", soundEntries_->getRecordCount(), " sounds, ", zoneMusic_.size(), " music sets, ",
             ambience_.size(), " ambiences, ", areas_.size(), " areas, ", impactRows_.size(),
             " weapon impact rows, ", footsteps_.size(), " footstep rows, ", itemGroupSounds_.size(),
             " item sound groups, ", vocalUiSounds_.size(), " vocal UI rows, ", emitterCount,
             " placed emitters on ", mapEmitters_.size(), " maps, ",
             advancedByKit_.size(), " advanced kits");
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

const SoundEntry* SoundData::vocalUiSound(uint32_t vocalUiEnum, uint32_t raceId, bool female, bool pissed) const {
    auto it = vocalUiSounds_.find((vocalUiEnum << 8) | (raceId & 0xFF));
    if (it == vocalUiSounds_.end()) return nullptr;
    const SoundEntry* entry = sound(it->second[(pissed ? 2 : 0) + (female ? 1 : 0)]);
    return entry && !entry->files.empty() ? entry : nullptr;
}

uint32_t SoundData::weatherSound(uint32_t weatherState) const {
    auto it = weatherSounds_.find(weatherState);
    return it == weatherSounds_.end() ? 0 : it->second;
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

const SoundEntry* SoundData::itemFoleyEntry(uint32_t itemDisplayId, bool pickup) const {
    const SoundEntry* entry = sound(itemFoleySound(itemDisplayId, pickup));
    return entry && !entry->files.empty() ? entry : nullptr;
}

const SoundEntry* SoundData::coinSound() const {
    // 3.3.5 has two rows with this name (120 and 895), both LootCoinSmall.wav
    // at volume 0.7, so which one the name resolves to does not matter.
    const SoundEntry* entry = soundByName("LOOTWINDOWCOINSOUND");
    return entry && !entry->files.empty() ? entry : nullptr;
}

const stock::AdvancedKit* SoundData::advancedKit(uint32_t soundId) const {
    auto it = advancedByKit_.find(soundId);
    return it == advancedByKit_.end() ? nullptr : &it->second;
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
    std::string dir = soundEntries_->getString(r, se::kDirectory);
    if (!dir.empty() && dir.back() != '\\') dir += '\\';
    // The kit ends at its first empty file (0x4cd4a0); the two rows with a
    // gap never play what comes after it.
    for (uint32_t f = 0; f < se::kFileCount; ++f) {
        std::string file = soundEntries_->getString(r, se::kFile0 + f);
        if (file.empty()) break;
        entry.files.push_back(dir + file);
        entry.freq.push_back(soundEntries_->getUInt32(r, se::kFreq0 + f));
    }
    entry.volume = soundEntries_->getFloat(r, se::kVolume);
    entry.flags = soundEntries_->getUInt32(r, se::kFlags);
    entry.minDistance = soundEntries_->getFloat(r, se::kMinDistance);
    entry.distanceCutoff = soundEntries_->getFloat(r, se::kDistanceCutoff);
    entry.advancedId = soundEntries_->getUInt32(r, se::kAdvanced);
    return &(soundCache_[id] = std::move(entry));
}

const std::string* SoundData::nextFile(const SoundEntry& entry) const {
    if (entry.files.empty()) return nullptr;
    std::vector<uint32_t> freq = entry.freq;
    // A row built by hand (tests, fallbacks) with no weights draws evenly.
    if (freq.size() != entry.files.size()) freq.assign(entry.files.size(), 1u);
    const int i = stock::pickKitFile(entry.bag, freq, rng_);
    return i < 0 ? nullptr : &entry.files[static_cast<size_t>(i)];
}

int SoundData::nextFileIndex(const SoundEntry& entry) const {
    const std::string* f = nextFile(entry);
    return f ? static_cast<int>(f - entry.files.data()) : -1;
}

const std::string* SoundData::nextFile(uint32_t soundId) const {
    const SoundEntry* entry = sound(soundId);
    return entry ? nextFile(*entry) : nullptr;
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

const WmoAreaSounds* SoundData::wmoArea(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const {
    auto it = wmoAreas_.find(wmoId);
    if (it == wmoAreas_.end()) return nullptr;
    return game::findWmoAreaRow(it->second, nameSet, groupId);
}

uint32_t SoundData::wmoAmbience(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const {
    const WmoAreaSounds* own = wmoArea(wmoId, nameSet, groupId);
    if (own && own->ambienceId != 0) return own->ambienceId;
    const WmoAreaSounds* building = groupId != -1 ? wmoArea(wmoId, nameSet, -1) : nullptr;
    return building ? building->ambienceId : 0;
}

uint32_t SoundData::wmoZoneMusic(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const {
    const WmoAreaSounds* own = wmoArea(wmoId, nameSet, groupId);
    if (own && own->zoneMusicId != 0) return own->zoneMusicId;
    const WmoAreaSounds* building = groupId != -1 ? wmoArea(wmoId, nameSet, -1) : nullptr;
    return building ? building->zoneMusicId : 0;
}

uint32_t SoundData::wmoIntro(uint32_t wmoId, uint16_t nameSet, int32_t groupId) const {
    const WmoAreaSounds* own = wmoArea(wmoId, nameSet, groupId);
    if (own && own->introId != 0) return own->introId;
    const WmoAreaSounds* building = groupId != -1 ? wmoArea(wmoId, nameSet, -1) : nullptr;
    return building ? building->introId : 0;
}

const SoundProviderPref* SoundData::providerPref(int32_t id) const {
    if (id <= 0) return nullptr;
    auto it = providerPrefs_.find(static_cast<uint32_t>(id));
    return it == providerPrefs_.end() ? nullptr : &it->second;
}

const SoundProviderPref* SoundData::providerPrefByName(std::string_view name) const {
    for (const auto& [id, p] : providerPrefs_) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

int32_t SoundData::areaReverb(uint32_t areaId, bool underwater) const {
    for (int depth = 0; areaId != 0 && depth < kMaxParentDepth; ++depth) {
        const AreaSounds* a = area(areaId);
        if (!a) return 0;
        const int32_t id = underwater ? a->reverbUnderwater : a->reverb;
        if (id > 0) return id;
        if (a->parent == areaId) return 0;
        areaId = a->parent;
    }
    return 0;
}

int32_t SoundData::wmoReverb(uint32_t wmoId, uint16_t nameSet, int32_t groupId, bool underwater) const {
    // A group's own row may carry only a name or music and leave the reverb
    // to the building's -1 row.
    const auto pick = [underwater](const WmoAreaSounds* row) {
        return row ? (underwater ? row->reverbUnderwater : row->reverb) : 0;
    };
    const int32_t own = pick(wmoArea(wmoId, nameSet, groupId));
    if (own > 0) return own;
    const int32_t building = groupId != -1 ? pick(wmoArea(wmoId, nameSet, -1)) : 0;
    return building > 0 ? building : 0;
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
    return soundDataRow(soundDataId);
}

const CreatureSounds* SoundData::modelSounds(uint32_t modelId) const {
    if (modelId == 0 || !modelData_ || !creatureSounds_) return nullptr;
    const int32_t modelRow = modelData_->findRecordById(modelId);
    if (modelRow < 0) return nullptr;
    return soundDataRow(modelData_->getUInt32(static_cast<uint32_t>(modelRow), cmd::kSound));
}

const CreatureSounds* SoundData::soundDataRow(uint32_t soundDataId) const {
    if (soundDataId == 0 || !creatureSounds_) return nullptr;
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
    cs.aggro = creatureSounds_->getUInt32(r, csd::kAggro);
    cs.petAttack = creatureSounds_->getUInt32(r, csd::kPetAttack);
    cs.petOrder = creatureSounds_->getUInt32(r, csd::kPetOrder);
    cs.petDismiss = creatureSounds_->getUInt32(r, csd::kPetDismiss);
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

const char* uiMessageSoundName(std::string_view messageKey) {
    // Every row of Wow.exe's UI message table (0xac8af0) whose sound column is
    // not "NONE", read out of the 3.3.5a (12340) binary. 0x5216f0 plays that
    // column by name when the row's vocal field holds 0x44, which every one of
    // these does. Sorted by key for the binary search below.
    struct Row { std::string_view key; const char* sound; };
    static constexpr Row kRows[] = {
        {"ERR_ARENA_TEAM_CREATE_S",                  "LEVELUP"},
        {"ERR_DECLINE_GROUP_S",                      "igPlayerInviteDecline"},
        {"ERR_DUEL_REQUESTED",                       "LEVELUP"},
        {"ERR_FRIEND_ONLINE_SS",                     "FRIENDJOINGAME"},
        {"ERR_GUILD_CREATE_S",                       "LEVELUP"},
        {"ERR_INVITED_TO_ARENA_TEAM",                "LEVELUP"},
        {"ERR_INVITED_TO_GUILD_SSS",                 "LEVELUP"},
        {"ERR_MEETING_STONE_IN_QUEUE_S",             "PVPENTERQUEUE"},
        {"ERR_NEWTAXIPATH",                          "TaxiNodeDiscovered"},
        {"ERR_PETITION_ALREADY_SIGNED",              "igPlayerInviteDecline"},
        {"ERR_PETITION_ALREADY_SIGNED_OTHER",        "igPlayerInviteDecline"},
        {"ERR_PETITION_CREATOR",                     "igPlayerInviteDecline"},
        {"ERR_PETITION_DECLINED_S",                  "igPlayerInviteDecline"},
        {"ERR_PETITION_FULL",                        "igPlayerInviteDecline"},
        {"ERR_PETITION_IN_GUILD",                    "igPlayerInviteDecline"},
        {"ERR_PETITION_NOT_ENOUGH_SIGNATURES",       "igPlayerInviteDecline"},
        {"ERR_PETITION_NOT_SAME_SERVER",             "igPlayerInviteDecline"},
        {"ERR_PETITION_RESTRICTED_ACCOUNT",          "igPlayerInviteDecline"},
        {"ERR_PETITION_SIGNED",                      "igPlayerInviteAccept"},
        {"ERR_PETITION_SIGNED_S",                    "igPlayerInviteAccept"},
        {"ERR_QUEST_ACCEPTED_S",                     "QUESTADDED"},
        {"ERR_QUEST_ALREADY_DONE",                   "igQuestFailed"},
        {"ERR_QUEST_ALREADY_DONE_DAILY",             "igQuestFailed"},
        {"ERR_QUEST_ALREADY_ON",                     "igQuestFailed"},
        {"ERR_QUEST_FAILED_BAG_FULL_S",              "igQuestFailed"},
        {"ERR_QUEST_FAILED_CAIS",                    "igQuestFailed"},
        {"ERR_QUEST_FAILED_EXPANSION",               "igQuestFailed"},
        {"ERR_QUEST_FAILED_LOW_LEVEL",               "igQuestFailed"},
        {"ERR_QUEST_FAILED_MAX_COUNT_S",             "igQuestFailed"},
        {"ERR_QUEST_FAILED_MISSING_ITEMS",           "igQuestFailed"},
        {"ERR_QUEST_FAILED_NOT_ENOUGH_MONEY",        "igQuestFailed"},
        {"ERR_QUEST_FAILED_S",                       "igQuestFailed"},
        {"ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I", "igQuestFailed"},
        {"ERR_QUEST_FAILED_WRONG_RACE",              "igQuestFailed"},
        {"ERR_QUEST_LOG_FULL",                       "igQuestFailed"},
        {"ERR_QUEST_MUST_CHOOSE",                    "igQuestFailed"},
        {"ERR_QUEST_NEED_PREREQS",                   "igQuestFailed"},
        {"ERR_QUEST_ONLY_ONE_TIMED",                 "igQuestFailed"},
        {"ERR_RECEIVE_ITEM_S",                       "ITEMGENERICSOUND"},
        {"ERR_TRADE_REQUEST_S",                      "LEVELUP"},
    };
    const auto* end = std::end(kRows);
    const auto* it = std::lower_bound(std::begin(kRows), end, messageKey,
                                      [](const Row& r, std::string_view k) { return r.key < k; });
    return (it != end && it->key == messageKey) ? it->sound : nullptr;
}

} // namespace audio
} // namespace wowee
