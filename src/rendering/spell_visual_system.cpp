#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/camera.hpp"
#include "rendering/camera_controller.hpp"
#include "rendering/placement_transform.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/terrain_manager.hpp"
#include "rendering/wmo_renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_data.hpp"
#include "audio/spell_sound_manager.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/gtc/constants.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace wowee {
namespace rendering {

namespace {
// SoundEntries.Flags: the row loops (precast and channel hums, missile
// flight loops - 1775 of 2037 precast kit sounds and 669 of 989 missile
// sounds carry it; 3759 of 3919 cast kit sounds do not).
constexpr uint32_t kSoundFlagLoops = 0x200;
// The stock spell models' sequences: Stand plays as the birth, Hold loops
// while the kit lasts, Decay plays as it ends (Renew_Base, Blizzard_Spawn).
constexpr uint32_t kAnimHold = 158;
constexpr uint32_t kAnimDecay = 159;
constexpr float kDegToRad = glm::pi<float>() / 180.0f;
// SpellVisual.Flags 0x200: MissileAttachment / MissileDestinationAttachment
// are M2 attachment ids rather than indices into the table below.
constexpr uint32_t kVisualFlagRawAttachments = 0x200;
// SpellVisualKit.Flags 0x20: a state kit loops its birth, not Hold.
constexpr uint32_t kKitFlagLoopStand = 0x20;
// Wow.exe 0xADAA20: SpellVisual's missile attachment index -> M2 attachment.
// Head, Chest, Base, SpellLeftHand, SpellRightHand, Breath, Special1-3,
// ChestBloodFront, ChestBloodBack, SpellHandOmni, SpellHandDirected,
// SpellRightHand again, and 47/48 (the feet). The first nine are the kit
// effect columns without the weapons (see kSlotAttach).
constexpr int32_t kMissileAttachmentIds[] = {20, 34, 19, 21, 22, 17, 23, 24, 25,
                                             15, 16, 37, 38, 22, 47, 48};
}  // namespace

SpellVisualSystem::~SpellVisualSystem() {
    if (motionLua_) {
        lua_close(motionLua_);
        motionLua_ = nullptr;
    }
}

void SpellVisualSystem::initialize(M2Renderer* m2Renderer, Renderer* renderer) {
    m2Renderer_ = m2Renderer;
    renderer_ = renderer;
}

void SpellVisualSystem::shutdown() {
    reset();
    unitStates_.clear();
    unitInstanceOf_ = nullptr;
    unitFeetOf_ = nullptr;
    m2Renderer_ = nullptr;
    renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
    if (motionLua_) {
        lua_close(motionLua_);
        motionLua_ = nullptr;
    }
    for (auto& [id, row] : motions_) row.ref = -2;
}

bool SpellVisualSystem::ensureLoaded() {
    if (!m2Renderer_) return false;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return false;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    return spellVisualDbcLoaded_;
}

// Load SpellVisual DBC chain: SpellVisualEffectName → SpellVisualKit → SpellVisual
// to build cast/impact M2 path lookup maps.
void SpellVisualSystem::loadSpellVisualDbc() {
    if (spellVisualDbcLoaded_) return;

    if (!cachedAssetManager_) {
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    }
    // Not an attempt. "Set early to prevent re-entry on failure" was the
    // intent, but there is no failure yet - only assets that have not
    // arrived. Latching here left spell visuals off for the session
    // whenever this was reached first.
    if (!cachedAssetManager_) return;
    spellVisualDbcLoaded_ = true;  // a real attempt follows; do not repeat it

    auto* layout = pipeline::getActiveDBCLayout();
    const pipeline::DBCFieldMap* svLayout  = layout ? layout->getLayout("SpellVisual")           : nullptr;
    const pipeline::DBCFieldMap* kitLayout = layout ? layout->getLayout("SpellVisualKit")        : nullptr;
    const pipeline::DBCFieldMap* fxLayout  = layout ? layout->getLayout("SpellVisualEffectName") : nullptr;

    constexpr uint32_t kNone = 0xFFFFFFFFu;
    uint32_t svCastKitField   = svLayout  ? (*svLayout)["CastKit"]       : 2;
    uint32_t svPrecastKitField = svLayout  ? (*svLayout)["PrecastKit"]    : 1;
    uint32_t svImpactKitField = svLayout  ? (*svLayout)["ImpactKit"]     : 3;
    uint32_t svMissileField   = svLayout  ? (*svLayout)["MissileModel"]  : 8;
    uint32_t fxFilePathField  = fxLayout  ? (*fxLayout)["FilePath"]       : 2;
    // Not in every layout file. ChannelKit is column 6 in every version
    // (Precast, Cast, Impact, State, StateDone, Channel); the missile
    // attachments are 3.3.5a's columns 16 and 10, used only on a file of that
    // shape (32 columns).
    uint32_t svChannelKitField = svLayout ? svLayout->tryField("ChannelKit") : kNone;
    if (svChannelKitField == kNone) svChannelKitField = 6;
    uint32_t svMissileAttachField = svLayout ? svLayout->tryField("MissileAttachment") : kNone;
    uint32_t svMissileDestField =
        svLayout ? svLayout->tryField("MissileDestinationAttachment") : kNone;

    // Every effect column of a kit, in KitSlot order. The weapon columns sit
    // between BreathEffect and SpecialEffect0 (3.3.5a: 9 and 10).
    static constexpr const char* kitFieldNames[SLOT_COUNT] = {
        "HeadEffect", "ChestEffect", "BaseEffect", "LeftHandEffect", "RightHandEffect",
        "BreathEffect", "LeftWeaponEffect", "RightWeaponEffect",
        "SpecialEffect0", "SpecialEffect1", "SpecialEffect2",
    };
    static constexpr uint32_t kitFieldFallback[SLOT_COUNT] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    uint32_t kitFields[SLOT_COUNT];
    for (size_t k = 0; k < SLOT_COUNT; ++k)
        kitFields[k] = kitLayout ? kitLayout->tryField(kitFieldNames[k]) : kitFieldFallback[k];
    if (kitLayout && kitFields[SLOT_LEFT_WEAPON] == kNone &&
        kitFields[SLOT_BREATH] != kNone && kitFields[SLOT_SPECIAL0] == kitFields[SLOT_BREATH] + 3) {
        kitFields[SLOT_LEFT_WEAPON]  = kitFields[SLOT_BREATH] + 1;
        kitFields[SLOT_RIGHT_WEAPON] = kitFields[SLOT_BREATH] + 2;
    }
    uint32_t kitAnimField = kitLayout ? kitLayout->tryField("AnimID") : kNone;
    if (kitAnimField == kNone && kitFields[SLOT_HEAD] == 3) kitAnimField = 2;
    uint32_t kitStartAnimField = kitLayout ? kitLayout->tryField("StartAnimID") : kNone;
    if (kitStartAnimField == kNone && kitFields[SLOT_HEAD] == 3) kitStartAnimField = 1;
    // 3.3.5a SpellVisualKit (38 columns): 14 WorldEffect, 15 SoundID. Used
    // from the layout when it names them, else only on a file of that shape.
    uint32_t kitWorldField = kitLayout ? kitLayout->tryField("WorldEffect") : kNone;
    uint32_t kitSoundField = kitLayout ? kitLayout->tryField("SoundID") : kNone;
    // 37 Flags: the kit's play flags (0x20 a state loops Stand instead of
    // Hold, 0x100 the caster keeps its weapons out).
    uint32_t kitFlagsField = kitLayout ? kitLayout->tryField("Flags") : kNone;
    // 16 ShakeID, 17-20 CharProc, 21-36 CharParamZero..Three (each [4]).
    uint32_t kitShakeField = kitLayout ? kitLayout->tryField("ShakeID") : kNone;
    uint32_t kitProcField = kitLayout ? kitLayout->tryField("CharProc") : kNone;

    // Load SpellVisualEffectName.dbc - ID → M2 path
    auto fxDbc = cachedAssetManager_->loadDBC("SpellVisualEffectName.dbc");
    if (!fxDbc || !fxDbc->isLoaded() || fxDbc->getFieldCount() <= fxFilePathField) {
        LOG_DEBUG("SpellVisual: SpellVisualEffectName.dbc unavailable (fc=",
                  fxDbc ? fxDbc->getFieldCount() : 0, ")");
        return;
    }
    std::unordered_map<uint32_t, std::string> effectPaths; // effectNameId → path
    for (uint32_t i = 0; i < fxDbc->getRecordCount(); ++i) {
        uint32_t id   = fxDbc->getUInt32(i, 0);
        std::string p = fxDbc->getString(i, fxFilePathField);
        if (id && !p.empty()) {
            // The DBC stores the extension the art was authored with; what
            // shipped is .m2.
            p = pipeline::modelPathToM2(p);
            effectPaths[id] = p;
        }
    }

    // Load SpellVisualKit.dbc - every effect slot of every kit. A kit used to
    // keep only the first slot that had anything, and the attachment point was
    // then guessed from words in the file name, so a kit with a chest and a
    // base effect showed one of them, and a hand effect named without "_hand"
    // stood at the caster's feet.
    auto kitDbc = cachedAssetManager_->loadDBC("SpellVisualKit.dbc");
    uint32_t kitEffects = 0;
    if (kitDbc && kitDbc->isLoaded()) {
        uint32_t fc = kitDbc->getFieldCount();
        if (fc == 38) {
            if (kitWorldField == kNone) kitWorldField = 14;
            if (kitSoundField == kNone) kitSoundField = 15;
            if (kitFlagsField == kNone) kitFlagsField = 37;
            if (kitShakeField == kNone) kitShakeField = 16;
            if (kitProcField == kNone) kitProcField = 17;
        }
        for (uint32_t i = 0; i < kitDbc->getRecordCount(); ++i) {
            uint32_t kitId = kitDbc->getUInt32(i, 0);
            if (!kitId) continue;
            KitRow row;
            for (uint8_t k = 0; k < SLOT_COUNT; ++k) {
                if (kitFields[k] >= fc) continue;
                const uint32_t eff = kitDbc->getUInt32(i, kitFields[k]);
                if (!eff) continue;
                auto fxIt = effectPaths.find(eff);
                if (fxIt == effectPaths.end()) continue;
                row.effects.push_back({.slot = k, .path = fxIt->second});
            }
            if (kitAnimField < fc) {
                const int32_t anim = kitDbc->getInt32(i, kitAnimField);
                row.animId = anim > 0 ? static_cast<uint32_t>(anim) : 0;
            }
            if (kitStartAnimField < fc) {
                const int32_t anim = kitDbc->getInt32(i, kitStartAnimField);
                row.startAnimId = anim > 0 ? static_cast<uint32_t>(anim) : 0;
            }
            if (kitWorldField < fc) {
                auto fxIt = effectPaths.find(kitDbc->getUInt32(i, kitWorldField));
                if (fxIt != effectPaths.end()) row.worldEffect = fxIt->second;
            }
            if (kitSoundField < fc) {
                const int32_t snd = kitDbc->getInt32(i, kitSoundField);
                row.soundId = snd > 0 ? static_cast<uint32_t>(snd) : 0;
            }
            if (kitFlagsField < fc) row.flags = kitDbc->getUInt32(i, kitFlagsField);
            if (kitShakeField < fc) {
                const int32_t shake = kitDbc->getInt32(i, kitShakeField);
                row.shakeId = shake > 0 ? static_cast<uint32_t>(shake) : 0;
            }
            // CharProc[j] at procField + j, its parameters four columns
            // apart after them (CharParamZero[j] .. CharParamThree[j]).
            if (kitProcField + 19 < fc) {
                for (uint32_t j = 0; j < 4; ++j) {
                    KitProc& proc = row.procs[j];
                    proc.type = kitDbc->getInt32(i, kitProcField + j);
                    for (uint32_t q = 0; q < 4; ++q)
                        proc.p[q] = kitDbc->getFloat(i, kitProcField + 4 + j + 4 * q);
                    if (proc.type >= 0 && proc.type != 14) row.hasProcs = true;
                }
            }
            kitEffects += static_cast<uint32_t>(row.effects.size());
            // A kit that is only a sound (Blizzard's persistent area: the
            // falling-ice clatter), only a world effect or only a procedure
            // (Chain Lightning's arcs) is still a kit.
            if (!row.effects.empty() || row.animId || row.startAnimId || row.soundId ||
                !row.worldEffect.empty() || row.hasProcs || row.shakeId)
                kits_[kitId] = std::move(row);
        }
    }

    // SpellVisualKitModelAttach.dbc (0 ID, 1 ParentSpellVisualKitID,
    // 2 SpellVisualEffectNameID, 3 AttachmentID, 4-6 Offset, 7 Yaw, 8 Pitch,
    // 9 Roll): more models a kit adds (Wow.exe 0x7FA9F0, 0x745230).
    if (auto attachDbc = cachedAssetManager_->loadDBC("SpellVisualKitModelAttach.dbc");
        attachDbc && attachDbc->isLoaded() && attachDbc->getFieldCount() >= 10) {
        for (uint32_t i = 0; i < attachDbc->getRecordCount(); ++i) {
            auto kIt = kits_.find(attachDbc->getUInt32(i, 1));
            auto fxIt = effectPaths.find(attachDbc->getUInt32(i, 2));
            if (fxIt == effectPaths.end()) continue;
            if (kIt == kits_.end()) {
                // A kit made only of attached models.
                kIt = kits_.emplace(attachDbc->getUInt32(i, 1), KitRow{}).first;
            }
            KitModelAttach a;
            a.path = fxIt->second;
            a.attachment = attachDbc->getInt32(i, 3);
            a.offset = glm::vec3(attachDbc->getFloat(i, 4), attachDbc->getFloat(i, 5),
                                 attachDbc->getFloat(i, 6));
            a.yaw = attachDbc->getFloat(i, 7);
            a.pitch = attachDbc->getFloat(i, 8);
            a.roll = attachDbc->getFloat(i, 9);
            kIt->second.attaches.push_back(std::move(a));
        }
    }

    // Load SpellVisual.dbc - visualId → its kits and missile
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    // The rest of 3.3.5a's SpellVisual (WoWDBDefs 3.3.5.12340, 32 columns):
    //  4 StateKit  5 StateDoneKit  7 HasMissile  9 MissilePathType
    // 11 MissileSound  17-20 MissileFollowGround Height/DropSpeed/Approach/
    // Flags (integers, hundredths of a yard)  21 MissileMotion
    // 23 InstantAreaKit  24 ImpactAreaKit  25 PersistentAreaKit.
    // Read from the layout when it names them, else only on a file of
    // that shape.
    auto field = [&](const char* name, uint32_t wotlk) -> uint32_t {
        uint32_t f = svLayout ? svLayout->tryField(name) : kNone;
        if (f == kNone && svFc == 32) f = wotlk;
        return f;
    };
    if (svFc == 32) {
        if (svMissileAttachField == kNone) svMissileAttachField = 16;
        if (svMissileDestField == kNone)   svMissileDestField = 10;
    }
    const uint32_t svStateKitField      = field("StateKit", 4);
    const uint32_t svStateDoneKitField  = field("StateDoneKit", 5);
    const uint32_t svTargetImpactField  = field("TargetImpactKit", 15);
    const uint32_t svCasterImpactField  = field("CasterImpactKit", 14);
    const uint32_t svFlagsField         = field("Flags", 13);
    const uint32_t svHasMissileField    = field("HasMissile", 7);
    const uint32_t svPathTypeField      = field("MissilePathType", 9);
    const uint32_t svMissileSoundField  = field("MissileSound", 11);
    const uint32_t svFgHeightField      = field("MissileFollowGroundHeight", 17);
    const uint32_t svFgDropField        = field("MissileFollowGroundDropSpeed", 18);
    const uint32_t svFgApproachField    = field("MissileFollowGroundApproach", 19);
    const uint32_t svFgFlagsField       = field("MissileFollowGroundFlags", 20);
    const uint32_t svMotionField        = field("MissileMotion", 21);
    const uint32_t svInstantAreaField   = field("InstantAreaKit", 23);
    const uint32_t svImpactAreaField    = field("ImpactAreaKit", 24);
    const uint32_t svPersistentAreaField = field("PersistentAreaKit", 25);
    auto readField = [&](uint32_t row, uint32_t field) -> uint32_t {
        return field < svFc ? svDbc->getUInt32(row, field) : 0u;
    };
    auto readInt = [&](uint32_t row, uint32_t field) -> int32_t {
        return field < svFc ? svDbc->getInt32(row, field) : 0;
    };
    uint32_t loadedMissile = 0;
    for (uint32_t i = 0; i < svDbc->getRecordCount(); ++i) {
        uint32_t vid = svDbc->getUInt32(i, 0);
        if (!vid) continue;
        VisualRow v;
        v.precastKit = readField(i, svPrecastKitField);
        v.castKit    = readField(i, svCastKitField);
        v.impactKit  = readField(i, svImpactKitField);
        v.channelKit = readField(i, svChannelKitField);
        v.stateKit     = readField(i, svStateKitField);
        v.stateDoneKit = readField(i, svStateDoneKitField);
        v.targetImpactKit = readField(i, svTargetImpactField);
        v.casterImpactKit = readField(i, svCasterImpactField);
        v.flags = readField(i, svFlagsField);
        v.instantAreaKit    = readField(i, svInstantAreaField);
        v.impactAreaKit     = readField(i, svImpactAreaField);
        v.persistentAreaKit = readField(i, svPersistentAreaField);
        // Where the layout has no HasMissile, a model is taken to mean one.
        v.hasMissile = svHasMissileField < svFc ? readField(i, svHasMissileField) != 0 : true;
        v.pathType = readInt(i, svPathTypeField);
        v.missileSound = readField(i, svMissileSoundField);
        v.motionId = readField(i, svMotionField);
        v.followGroundFlags = readField(i, svFgFlagsField);
        v.followGroundHeight    = static_cast<float>(readInt(i, svFgHeightField)) / 100.0f;
        v.followGroundDropSpeed = static_cast<float>(readInt(i, svFgDropField)) / 100.0f;
        v.followGroundApproach  = static_cast<float>(readInt(i, svFgApproachField)) / 100.0f;
        // The DBC's attachment columns are indices into the stock table
        // (Wow.exe 0xADAA20, read at 0x7FAA40 for the source and 0x732FF0 for
        // the destination), or raw attachment ids with Flags 0x200.
        auto attachmentOf = [&](int32_t value, int32_t fallback) -> int32_t {
            if (value < 0) return fallback;
            if (v.flags & kVisualFlagRawAttachments) return value;
            return value < static_cast<int32_t>(std::size(kMissileAttachmentIds))
                ? kMissileAttachmentIds[value] : fallback;
        };
        if (svMissileAttachField < svFc)
            v.missileAttachment = attachmentOf(svDbc->getInt32(i, svMissileAttachField), -1);
        if (svMissileDestField < svFc)
            v.missileDestination = attachmentOf(svDbc->getInt32(i, svMissileDestField), 34);
        // The missile flies from caster to target; it is never a stand-in
        // for a missing cast or impact kit.
        if (const int32_t raw = readInt(i, svMissileField); raw == -1 || raw == -2)
            v.weaponMissile = raw;
        if (const uint32_t eff = readField(i, svMissileField)) {
            auto fxIt = effectPaths.find(eff);
            if (fxIt != effectPaths.end()) { v.missilePath = fxIt->second; ++loadedMissile; }
        }
        visuals_[vid] = std::move(v);
    }
    LOG_INFO("SpellVisual: loaded ", visuals_.size(), " visuals, ", kits_.size(), " kits (",
             kitEffects, " effects), ", loadedMissile, " missiles");

    // SpellMissileMotion.dbc (3.3.5a: 0 ID, 1 Name, 2 ScriptBody, 3 Flags,
    // 4 MissileCount) - the Lua that bends a missile's path: "Parabola" (13)
    // for thrown weapons and lobbed bombs, spins, spirals, missile swarms.
    auto motionDbc = cachedAssetManager_->loadDBC("SpellMissileMotion.dbc");
    if (motionDbc && motionDbc->isLoaded() && motionDbc->getFieldCount() >= 5) {
        for (uint32_t i = 0; i < motionDbc->getRecordCount(); ++i) {
            const uint32_t id = motionDbc->getUInt32(i, 0);
            if (!id) continue;
            MotionRow row;
            row.name = motionDbc->getString(i, 1);
            row.script = motionDbc->getString(i, 2);
            row.missileCount = std::clamp<uint32_t>(motionDbc->getUInt32(i, 4), 1u, 10u);
            if (!row.script.empty()) motions_[id] = std::move(row);
        }
        LOG_INFO("SpellVisual: loaded ", motions_.size(), " missile motion scripts");
    }
}

// ---------------------------------------------------------------------------
// Kit slot → model attachment point
// ---------------------------------------------------------------------------
// The stock client's own mapping (CGUnit_C::PlaySpellVisualKit, Wow.exe
// 0x745230, and 0x73A6C0 for the weapons): head 20, chest 34 (Chest - not 15,
// ChestBloodFront, which is the blood spray's point and turned with it),
// base 19, hands 21/22, breath 17, weapons 2/1 (the hands that hold them),
// specials 23/24/25 (Special1-3). The later ids are this client's fallbacks
// for a model that lacks the first: 11 (Helm) is missing on creatures, and
// few models carry Special1-3, whose effects then stand at the base.
namespace {
struct SlotAttach { uint32_t ids[3]; float fallbackHeight; };
constexpr SlotAttach kSlotAttach[SpellVisualSystem::SLOT_COUNT] = {
    /* HEAD         */ {{20, 11, 0},  1.9f},
    /* CHEST        */ {{34, 15, 0},  1.3f},
    /* BASE         */ {{19, 0, 0},   0.0f},
    /* LEFT_HAND    */ {{21, 2, 0},   1.0f},
    /* RIGHT_HAND   */ {{22, 1, 0},   1.0f},
    /* BREATH       */ {{17, 20, 11}, 1.7f},
    /* LEFT_WEAPON  */ {{2, 21, 0},   1.0f},
    /* RIGHT_WEAPON */ {{1, 22, 0},   1.0f},
    /* SPECIAL0     */ {{23, 19, 0},  0.0f},
    /* SPECIAL1     */ {{24, 19, 0},  0.0f},
    /* SPECIAL2     */ {{25, 19, 0},  0.0f},
};
// Base and special effects stand on the ground under the unit (19 Base).
// A 0 ends a list: id 0 (Shield/MountMain) is never a spell slot.
constexpr bool usesId(uint32_t id) { return id != 0; }
} // namespace

bool SpellVisualSystem::slotTransform(uint8_t slot, uint32_t attachInstanceId,
                                      const glm::vec3& feetPos, glm::mat4& out) const {
    if (slot >= SLOT_COUNT) slot = SLOT_BASE;
    if (attachInstanceId != 0 && renderer_) {
        if (auto* cr = renderer_->getCharacterRenderer()) {
            for (uint32_t id : kSlotAttach[slot].ids) {
                if (!usesId(id)) break;
                if (cr->getAttachmentTransform(attachInstanceId, id, out)) return true;
            }
        }
    }
    out = glm::translate(glm::mat4(1.0f),
                         feetPos + glm::vec3(0.0f, 0.0f, kSlotAttach[slot].fallbackHeight));
    return false;
}

bool SpellVisualSystem::attachmentTransform(int32_t attachmentId, uint32_t attachInstanceId,
                                            const glm::vec3& feetPos, float fallbackHeight,
                                            glm::mat4& out) const {
    if (attachInstanceId != 0 && renderer_) {
        if (auto* cr = renderer_->getCharacterRenderer()) {
            // Wow.exe 0x7022D0: the attachment asked for, else 15, else 19.
            for (int32_t id : {attachmentId, 15, 19}) {
                if (id < 0) continue;
                if (cr->getAttachmentTransform(attachInstanceId, static_cast<uint32_t>(id), out))
                    return true;
            }
        }
    }
    out = glm::translate(glm::mat4(1.0f), feetPos + glm::vec3(0.0f, 0.0f, fallbackHeight));
    return false;
}

// One kit on one unit (or at one point): each slot the kit fills gets its
// own model at that slot's attachment, riding it from then on; the world
// effect stands where the kit was played; the kit's sound plays at the unit.
void SpellVisualSystem::spawnKit(uint32_t kitId, const KitSpawn& spawn) {
    if (kitId == 0 || !ensureLoaded()) return;
    auto kitIt = kits_.find(kitId);
    if (kitIt == kits_.end()) return;
    const KitRow& kit = kitIt->second;

    float longest = 0.0f;  // the kit's models' lifetime, for a looping sound
    const size_t before = activeSpellVisuals_.size();
    for (const auto& fx : kit.effects) {
        // At a point on the ground only the base effect has a place to be.
        if (spawn.atPoint && fx.slot != SLOT_BASE) continue;
        spawnKitModel(kitId, fx.path, fx.slot, /*world=*/false, spawn);
    }
    if (!kit.worldEffect.empty())
        spawnKitModel(kitId, kit.worldEffect, SLOT_BASE, /*world=*/true, spawn);
    // SpellVisualKitModelAttach: at an attachment of the unit, or at the
    // kit's point (-1). At a point only the latter has a place (0x745230).
    for (const auto& a : kit.attaches) {
        if (spawn.atPoint && a.attachment >= 0) continue;
        spawnKitModel(kitId, a.path, SLOT_BASE, /*world=*/a.attachment < 0, spawn, &a);
    }
    for (size_t i = before; i < activeSpellVisuals_.size(); ++i)
        longest = std::max(longest, activeSpellVisuals_[i].duration);

    // The kit's camera shake, from where it plays (0x73B140 -> 0x7FA620).
    if (kit.shakeId != 0) playCameraShake(kit.shakeId, spawn.feetPos);

    // Its character procedures, which last as long as a carrier visual of
    // the kit's own lifetime: a state's until the aura goes, a channel's
    // until it ends, a cast's or an impact's as long as its models.
    if (kit.hasProcs) {
        if (const uint32_t token = applyKitProcs(kitId, kit, spawn)) {
            SpellVisualInstance carrier;
            carrier.duration = spawn.lifetime > 0.0f ? spawn.lifetime
                             : (longest > 0.0f ? longest : SPELL_VISUAL_DEFAULT_DURATION);
            carrier.isPrecast = spawn.isPrecast;
            carrier.ownerGuid = spawn.ownerGuid;
            carrier.stateGuid = spawn.stateGuid;
            carrier.stateVisual = spawn.stateVisual;
            carrier.areaOwner = spawn.areaOwner;
            carrier.procToken = token;
            activeSpellVisuals_.push_back(carrier);
        }
    }

    if (kit.soundId != 0) {
        glm::mat4 xf;
        const bool attached = slotTransform(SLOT_CHEST, spawn.attachInstanceId, spawn.feetPos, xf);
        const glm::vec3 at = spawn.attachInstanceId ? glm::vec3(xf[3]) : spawn.feetPos;
        bool loops = false;
        const bool own = spawn.attachInstanceId != 0 && renderer_ &&
                         spawn.attachInstanceId == renderer_->getCharacterInstanceId();
        const uint32_t handle = playSoundEntry(kit.soundId, at, /*keepLoop=*/true, &loops, own);
        if (handle != 0) {
            // A looping sound (the precast hum, a channel, Blizzard's clatter)
            // lasts as long as the kit does and fades out with it.
            SpellVisualInstance sv;
            sv.duration = spawn.lifetime > 0.0f ? spawn.lifetime
                        : (longest > 0.0f ? longest : SPELL_VISUAL_DEFAULT_DURATION);
            sv.isPrecast = spawn.isPrecast;
            sv.slot = SLOT_CHEST;
            sv.attachInstanceId = attached ? spawn.attachInstanceId : 0u;
            sv.ownerGuid = spawn.ownerGuid;
            sv.soundHandle = handle;
            sv.soundPos = at;
            sv.stateGuid = spawn.stateGuid;
            sv.stateVisual = spawn.stateVisual;
            sv.areaOwner = spawn.areaOwner;
            activeSpellVisuals_.push_back(sv);
        }
    }
}

bool SpellVisualSystem::spawnKitModel(uint32_t kitId, const std::string& path, uint8_t slot,
                                      bool world, const KitSpawn& spawn,
                                      const KitModelAttach* attach) {
    const uint32_t modelId = loadSpellModel(path);
    if (modelId == 0) return false;
    glm::mat4 xf;
    bool attached = false;
    // An attached model's own offset and turn (radians) from its point.
    glm::mat4 local(1.0f);
    if (attach) {
        local = glm::translate(glm::mat4(1.0f), attach->offset);
        local = glm::rotate(local, attach->yaw, glm::vec3(0.0f, 0.0f, 1.0f));
        local = glm::rotate(local, attach->pitch, glm::vec3(0.0f, 1.0f, 0.0f));
        local = glm::rotate(local, attach->roll, glm::vec3(1.0f, 0.0f, 0.0f));
    }
    if (world) xf = glm::translate(glm::mat4(1.0f), spawn.feetPos) * local;
    else if (attach)
        attached = attachmentTransform(attach->attachment, spawn.attachInstanceId, spawn.feetPos,
                                       1.0f, xf);
    else attached = slotTransform(slot, spawn.attachInstanceId, spawn.feetPos, xf);
    if (attach && !world) xf = xf * local;
    const uint32_t instanceId =
        m2Renderer_->createInstance(modelId, glm::vec3(xf[3]), glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: createInstance failed for kit ", kitId, " model ", path);
        return false;
    }
    m2Renderer_->markInstanceSpellVisual(instanceId);
    if (attached) m2Renderer_->setInstanceTransform(instanceId, xf);
    m2Renderer_->restartInstanceAnimation(instanceId);

    SpellVisualInstance sv;
    sv.instanceId = instanceId;
    sv.modelId = modelId;
    sv.effectPos = glm::vec3(xf[3]);
    const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
    sv.birthDuration = animDurMs / 1000.0f;
    sv.hasHold = m2Renderer_->hasAnimation(instanceId, kAnimHold);
    sv.hasDecay = m2Renderer_->hasAnimation(instanceId, kAnimDecay);
    // A precast kit replays its birth for as long as the cast lasts, and a
    // state kit flagged 0x20 loops it too; the others go on to Hold.
    if (auto kIt = kits_.find(kitId); spawn.loopBirth ||
        (kIt != kits_.end() && (kIt->second.flags & kKitFlagLoopStand)))
        sv.noHold = true;
    float duration = spawn.lifetime;
    if (duration <= 0.0f) {
        duration = (animDurMs > 100.0f)
            ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
            : SPELL_VISUAL_DEFAULT_DURATION;
        // A one-shot kit (a cast or an impact, timed by its own sequence)
        // plays that sequence once. M2 sequences loop, and the model used to
        // start over as it reached its end - the frame the kit retires on -
        // so its first frames played again through the quarter-second fade:
        // a second Judgement hammer, a Seal of Righteousness impact twice.
        // Precast, channel, area and state kits are given a lifetime and
        // loop as authored for as long as they last.
        m2Renderer_->holdInstanceAnimationAtEnd(instanceId);
    }
    sv.duration = duration;
    sv.isPrecast = spawn.isPrecast;
    sv.slot = slot;
    sv.attachInstanceId = attached ? spawn.attachInstanceId : 0u;
    sv.ownerGuid = spawn.ownerGuid;
    sv.stateGuid = spawn.stateGuid;
    sv.stateVisual = spawn.stateVisual;
    sv.areaOwner = spawn.areaOwner;
    if (attach && !world) {
        sv.rawAttachment = attach->attachment;
        sv.attachLocal = local;
    }
    activeSpellVisuals_.push_back(sv);
    LOG_DEBUG("SpellVisual: kit ", kitId, " slot ", static_cast<int>(slot), world ? " world" : "",
              " model ", path, " attached=", attached, " duration=", duration, "s");
    return true;
}

uint32_t SpellVisualSystem::playSoundEntry(uint32_t soundEntryId, const glm::vec3& pos,
                                           bool keepLoop, bool* loops, bool own) {
    if (loops) *loops = false;
    if (soundEntryId == 0 || !renderer_) return 0;
    auto* ac = renderer_->getAudioCoordinator();
    const audio::SoundData* data = ac ? ac->getSoundData() : nullptr;
    if (!data) return 0;
    const audio::SoundEntry* entry = data->sound(soundEntryId);
    if (!entry || entry->files.empty()) return 0;
    const bool looping = (entry->flags & kSoundFlagLoops) != 0;
    if (loops) *loops = looping;
    // The spell sound slider, as the school sounds it replaces honoured.
    float scale = 1.0f;
    if (auto* ssm = ac->getSpellSoundManager()) scale = ssm->getVolumeScale();
    const float volume = entry->volume * scale;
    if (volume <= 0.0f) return 0;
    const auto& file = entry->files[std::uniform_int_distribution<size_t>(
        0, entry->files.size() - 1)(rng_)];
    // The row's own MinDistance and DistanceCutoff, as every positional kit
    // plays (Wow.exe 0x4c6a40 -> set3DMinMaxDistance).
    const float maxDistance = entry->distanceCutoff > 0.0f ? entry->distanceCutoff : audio::stock::kDefaultCutoff;
    audio::SoundOptions opts;
    opts.minDistance = entry->minDistance;
    opts.kitId = entry->id;
    // On the local character: in the head at 0.65 (0x7469c0 kit sounds,
    // 0x6f7b00 an effect model's $SND).
    opts.ownCharacter = own;
    auto& engine = audio::AudioEngine::instance();
    if (looping && keepLoop)
        return engine.playSound3DStoppable(file, pos, volume, maxDistance, /*loop=*/true, entry->minDistance, opts);
    engine.playSound3D(file, pos, volume, 1.0f, maxDistance, opts);
    return 0;
}

void SpellVisualSystem::playSpellVisualKit(uint32_t kitId, const glm::vec3& worldPosition,
                                           uint32_t attachInstanceId) {
    KitSpawn s;
    s.feetPos = worldPosition;
    s.attachInstanceId = attachInstanceId;
    spawnKit(kitId, s);
}

bool SpellVisualSystem::playEffectModel(const std::string& modelPath, const glm::vec3& feetPos,
                                        uint32_t attachInstanceId) {
    if (!m2Renderer_ || modelPath.empty()) return false;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return false;
    KitSpawn s;
    s.feetPos = feetPos;
    s.attachInstanceId = attachInstanceId;
    return spawnKitModel(0, modelPath, SLOT_BASE, /*world=*/false, s);
}

bool SpellVisualSystem::visualHasKitSound(uint32_t visualId, KitSoundMoment moment) {
    if (visualId == 0 || !ensureLoaded()) return false;
    auto vIt = visuals_.find(visualId);
    if (vIt == visuals_.end()) return false;
    const VisualRow& v = vIt->second;
    auto kitSounds = [&](uint32_t kitId) {
        if (kitId == 0) return false;
        auto kIt = kits_.find(kitId);
        return kIt != kits_.end() && kIt->second.soundId != 0;
    };
    switch (moment) {
        case KitSoundMoment::PRECAST: return kitSounds(v.precastKit) || kitSounds(v.channelKit);
        case KitSoundMoment::CAST:    return kitSounds(v.castKit) || (v.hasMissile && v.missileSound != 0);
        case KitSoundMoment::IMPACT:  return kitSounds(v.impactKit) || kitSounds(v.targetImpactKit) ||
                                             kitSounds(v.impactAreaKit);
        case KitSoundMoment::ANY:
            return kitSounds(v.precastKit) || kitSounds(v.channelKit) || kitSounds(v.castKit) ||
                   (v.hasMissile && v.missileSound != 0) || kitSounds(v.impactKit) ||
                   kitSounds(v.targetImpactKit) || kitSounds(v.impactAreaKit) ||
                   kitSounds(v.instantAreaKit) || kitSounds(v.persistentAreaKit) ||
                   kitSounds(v.stateKit) || kitSounds(v.stateDoneKit);
    }
    return false;
}

SpellVisualSystem::KitAnimations SpellVisualSystem::getKitAnimations(uint32_t visualId) {
    KitAnimations out;
    if (visualId == 0) return out;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto vIt = visuals_.find(visualId);
    if (vIt == visuals_.end()) return out;
    auto anim = [&](uint32_t kitId) -> uint32_t {
        auto kIt = kits_.find(kitId);
        return kIt != kits_.end() ? kIt->second.animId : 0u;
    };
    out.hasPrecastKit = vIt->second.precastKit != 0;
    if (auto kIt = kits_.find(vIt->second.precastKit); kIt != kits_.end())
        out.precastStart = kIt->second.startAnimId;
    out.precast = anim(vIt->second.precastKit);
    out.cast    = anim(vIt->second.castKit);
    out.channel = anim(vIt->second.channelKit);
    out.state   = anim(vIt->second.stateKit);
    out.impact  = anim(vIt->second.targetImpactKit ? vIt->second.targetImpactKit
                                                   : vIt->second.impactKit);
    return out;
}

void SpellVisualSystem::playSpellVisualPrecast(uint32_t visualId, const glm::vec3& worldPosition,
                                                uint32_t durationMs, uint32_t attachInstanceId,
                                                uint64_t casterGuid, bool channel) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    auto vIt = visuals_.find(visualId);
    if (vIt == visuals_.end()) return;
    // No stand-in: a spell without a precast (or channel) kit shows nothing
    // while it builds, the way the stock client does. Playing the cast kit
    // here instead showed it twice - now and again on SPELL_GO.
    const uint32_t kitId = channel ? vIt->second.channelKit : vIt->second.precastKit;
    if (kitId == 0) return;

    // A caster starting a new cast has finished with the old build-up.
    if (casterGuid != 0) cancelPrecastVisuals(casterGuid);

    // Kept until the cast resolves; the timer is only a net for a resolution
    // that never arrives (out of range of the SPELL_GO, a dropped packet).
    // An instant (durationMs 0) gets a short one: its SPELL_GO follows at once.
    KitSpawn s;
    s.feetPos = worldPosition;
    s.attachInstanceId = attachInstanceId;
    s.isPrecast = true;
    s.loopBirth = !channel;  // a channel kit is a state kit to the stock client
    s.ownerGuid = casterGuid;
    s.unitGuid = casterGuid;
    s.visualId = visualId;
    s.channelKit = channel;
    s.lifetime = (durationMs > 0)
        ? std::min(static_cast<float>(durationMs) / 1000.0f + 1.0f, SPELL_PRECAST_MAX_DURATION)
        : 1.0f;
    spawnKit(kitId, s);
}

// The cast, impact and missile models load the same way: a model ID per path,
// a negative cache for paths that failed, and the skin beside a WotLK model.
uint32_t SpellVisualSystem::loadSpellModel(const std::string& modelPath) {
    // Get or assign a model ID for this path
    auto midIt = spellVisualModelIds_.find(modelPath);
    uint32_t modelId = 0;
    if (midIt != spellVisualModelIds_.end()) {
        modelId = midIt->second;
    } else {
        if (nextSpellVisualModelId_ >= 999800) {
            LOG_WARNING("SpellVisual: model ID pool exhausted");
            return 0;
        }
        modelId = nextSpellVisualModelId_++;
        spellVisualModelIds_[modelPath] = modelId;
    }

    // Skip models that have previously failed to load (avoid repeated I/O)
    if (spellVisualFailedModels_.count(modelId)) {
        LOG_DEBUG("SpellVisual: model in failed-cache, skipping: ", modelPath);
        return 0;
    }

    // Load the M2 model if not already loaded
    if (!m2Renderer_->hasModel(modelId)) {
        auto m2Data = cachedAssetManager_->readFile(modelPath);
        if (m2Data.empty()) {
            LOG_WARNING("SpellVisual: could not read model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return 0;
        }
        LOG_INFO("SpellVisual: M2 data read OK, size=", m2Data.size(), " bytes");
        pipeline::M2Model model = pipeline::M2Loader::load(m2Data);
        if (model.name.empty()) model.name = modelPath;
        LOG_INFO("SpellVisual: M2 parsed: verts=", model.vertices.size(),
                 " bones=", model.bones.size(), " particles=", model.particleEmitters.size(),
                 " ribbons=", model.ribbonEmitters.size());
        if (model.vertices.empty() && model.particleEmitters.empty()) {
            LOG_WARNING("SpellVisual: empty model: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return 0;
        }
        // Load skin file for WotLK-format M2s
        if (model.version >= 264) {
            std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skinData = cachedAssetManager_->readFile(skinPath);
            if (!skinData.empty()) pipeline::M2Loader::loadSkin(skinData, model);
        }
        if (!m2Renderer_->loadModel(model, modelId)) {
            LOG_WARNING("SpellVisual: failed to load model to GPU: ", modelPath);
            spellVisualFailedModels_.insert(modelId);
            return 0;
        }
        m2Renderer_->markModelAsSpellEffect(modelId);
        std::vector<EffectSound> sounds;
        sounds.reserve(model.effectSounds.size());
        for (const auto& s : model.effectSounds) sounds.push_back({s.soundEntryId, s.timeMs});
        if (sounds.empty()) modelEffectSounds_.erase(modelId);
        else modelEffectSounds_[modelId] = std::move(sounds);
        std::vector<EffectSound> shakes;
        for (const auto& sh : model.effectShakes) shakes.push_back({sh.soundEntryId, sh.timeMs});
        if (shakes.empty()) modelEffectShakes_.erase(modelId);
        else modelEffectShakes_[modelId] = std::move(shakes);
        LOG_INFO("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    }
    return modelId;
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId,
                                         bool targetIsCaster, uint64_t unitGuid,
                                         const std::vector<uint64_t>* targets) {
    if (!m2Renderer_ || visualId == 0) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();

    auto vIt = visuals_.find(visualId);
    if (vIt == visuals_.end()) return;
    // The kit asked for and no other. A missing cast kit used to borrow the
    // impact kit (and the reverse), so Sinister Strike - whose cast kit is an
    // animation only - burst its impact on the rogue as well as the target.
    const VisualRow& v = vIt->second;
    uint32_t kitId = v.castKit;
    if (useImpactKit) {
        // One impact per unit hit (Wow.exe 0x800D00): TargetImpactKit in
        // place of ImpactKit on a target - Blizzard's tick spell 42208 has
        // only that one - CasterImpactKit on the caster. Both used to play,
        // so the 19 visuals naming two different kits burst twice.
        const uint32_t own = targetIsCaster ? v.casterImpactKit : v.targetImpactKit;
        kitId = own ? own : v.impactKit;
    }
    KitSpawn s;
    s.feetPos = worldPosition;
    s.attachInstanceId = attachInstanceId;
    s.unitGuid = unitGuid;
    s.visualId = visualId;
    s.targets = targets;
    spawnKit(kitId, s);
}

uint32_t SpellVisualSystem::visualFlags(uint32_t visualId) {
    if (visualId == 0 || !ensureLoaded()) return 0;
    auto it = visuals_.find(visualId);
    return it != visuals_.end() ? it->second.flags : 0u;
}

bool SpellVisualSystem::hasSpellMissile(uint32_t visualId) {
    if (visualId == 0 || !ensureLoaded()) return false;
    auto it = visuals_.find(visualId);
    return it != visuals_.end() && it->second.hasMissile &&
           (!it->second.missilePath.empty() || it->second.weaponMissile != 0);
}

void SpellVisualSystem::playSpellAreaVisual(uint32_t visualId, const glm::vec3& groundPos,
                                            float lifetimeSec, uint64_t channelOwnerGuid,
                                            bool impactNow, float radius) {
    if (visualId == 0 || !ensureLoaded()) return;
    auto vIt = visuals_.find(visualId);
    if (vIt == visuals_.end()) return;
    const VisualRow& v = vIt->second;
    KitSpawn s;
    s.feetPos = groundPos;
    s.atPoint = true;
    s.visualId = visualId;
    s.areaRadius = radius;
    // Shadowfury's burst, Mass Dispel's ring: once, where it was cast.
    spawnKit(v.instantAreaKit, s);

    const float life = (std::isfinite(lifetimeSec) && lifetimeSec > 0.0f)
        ? std::min(lifetimeSec, SPELL_AREA_MAX_DURATION) : 0.0f;
    const bool persists = v.persistentAreaKit != 0 && (life > 0.0f || channelOwnerGuid != 0);
    KitSpawn area = s;
    area.lifetime = life > 0.0f ? life : SPELL_PRECAST_MAX_DURATION;
    area.areaOwner = channelOwnerGuid;
    // The impact area kit of a lasting area lasts with it: Blizzard's falling
    // ice is its ImpactAreaKit (Blizzard_Spawn: Stand, Hold, Decay), the
    // persistent kit being only the clatter. Otherwise it plays once.
    if (impactNow) spawnKit(v.impactAreaKit, persists ? area : s);
    // Flamestrike's flames, Consecration's glow, Blizzard's clatter: for as
    // long as the area lasts - the spell's duration, or the channel.
    if (persists) spawnKit(v.persistentAreaKit, area);
}

void SpellVisualSystem::cancelChannelAreaVisuals(uint64_t casterGuid) {
    if (!m2Renderer_ || casterGuid == 0) return;
    for (auto& sv : activeSpellVisuals_)
        if (sv.areaOwner == casterGuid && !sv.retiring) retireVisual(sv);
}

bool SpellVisualSystem::playSpellMissile(uint32_t visualId, const glm::vec3& start,
                                         const glm::vec3& end, float speed, bool impactAtEnd,
                                         uint32_t casterInstanceId, uint32_t targetInstanceId,
                                         bool groundTarget, float launchDelay,
                                         uint32_t releaseAnim, uint64_t targetGuid) {
    if (!m2Renderer_ || !hasSpellMissile(visualId)) return false;
    if (!cachedAssetManager_) return false;
    // Speed 0 flies nothing. 454 spells with a real missile (HasMissile and
    // a model) have Speed 0 - Flame Shock, Frostbrand Attack, the death
    // knight's Death Coil 47541 - and they hit at once on the server. Where
    // the stock client shows a missile for one, the spell it triggers has the
    // speed: Death Coil's damage spell 47632 flies the same visual at 24.
    if (!std::isfinite(speed) || speed <= 0.0f) return false;
    if (visuals_.at(visualId).weaponMissile == 0 && loadSpellModel(visuals_.at(visualId).missilePath) == 0)
        return false;

    PendingMissile p;
    p.visualId = visualId;
    p.start = start;
    p.end = end;
    p.speed = speed;
    p.impactAtEnd = impactAtEnd;
    p.casterInstanceId = casterInstanceId;
    p.targetInstanceId = targetInstanceId;
    p.groundTarget = groundTarget;
    p.releaseAnim = releaseAnim;
    p.targetGuid = targetGuid;
    p.delay = std::clamp(std::isfinite(launchDelay) ? launchDelay : 0.0f, 0.0f,
                         SPELL_RELEASE_MAX_WAIT);
    // Held until the cast animation lets it go: Frostbolt leaves the left
    // hand 200 ms into SpellCastDirected, not as the cast bar fills.
    if (p.delay > 0.0f) {
        pendingMissiles_.push_back(p);
        return true;
    }
    return launchSpellMissile(p);
}

bool SpellVisualSystem::launchSpellMissile(const PendingMissile& p) {
    const uint32_t visualId = p.visualId;
    const glm::vec3& start = p.start;
    const glm::vec3& end = p.end;
    const uint32_t casterInstanceId = p.casterInstanceId;
    const uint32_t targetInstanceId = p.targetInstanceId;
    const bool groundTarget = p.groundTarget;
    auto vIt = visuals_.find(visualId);
    if (vIt == visuals_.end()) return false;
    const VisualRow& v = vIt->second;
    if (v.weaponMissile != 0) return launchWeaponMissile(p);
    const uint32_t modelId = loadSpellModel(v.missilePath);
    if (modelId == 0) return false;

    // Launch point (Wow.exe 0x7FAA40, 0x734390): the caster attachment
    // SpellVisual names; else, for the default (-1, nearly every bolt), the
    // point of the release event that let it go ($CSL: the left hand
    // thrusting forward in SpellCastDirected); else between the hands.
    glm::vec3 launch;
    glm::mat4 xf;
    uint32_t releaseMs = 0;
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (v.missileAttachment >= 0 && casterInstanceId != 0 && cr &&
        cr->getAttachmentTransform(casterInstanceId, static_cast<uint32_t>(v.missileAttachment), xf)) {
        launch = glm::vec3(xf[3]);
    } else if (casterInstanceId != 0 && cr && p.releaseAnim != 0 &&
               cr->getCastReleaseEvent(casterInstanceId, p.releaseAnim, releaseMs, &launch)) {
        // launch is the event's point
    } else {
        glm::mat4 l, r;
        const bool hasL = slotTransform(SLOT_LEFT_HAND, casterInstanceId, start, l);
        const bool hasR = slotTransform(SLOT_RIGHT_HAND, casterInstanceId, start, r);
        if (hasL && hasR)  launch = (glm::vec3(l[3]) + glm::vec3(r[3])) * 0.5f;
        else if (hasR)     launch = glm::vec3(r[3]);
        else if (hasL)     launch = glm::vec3(l[3]);
        else { slotTransform(SLOT_CHEST, 0, start, xf); launch = glm::vec3(xf[3]); }
    }
    const int32_t destAttachment = v.missileDestination;
    glm::vec3 aim = end;  // a ground point is aimed at as it is
    if (!groundTarget) {
        attachmentTransform(destAttachment, targetInstanceId, end,
                            kSlotAttach[SLOT_CHEST].fallbackHeight, xf);
        aim = glm::vec3(xf[3]);
    }
    const glm::vec3 d = aim - launch;
    const float distance = glm::length(d);
    const glm::vec3 dir = distance > 1e-4f ? d / distance : glm::vec3(1, 0, 0);

    // A missile that waited for its release still lands when the server's
    // flight time says: the time left is the flight less the wait, and the
    // speed is what covers the distance in it - at most twice the spell's
    // (Wow.exe 0x7022D0).
    float speed = p.speed;
    if (p.waited > 0.0f && distance > 0.0f) {
        const float left = distance / p.speed - p.waited;
        speed = left > 0.0f ? std::min(distance / left, p.speed * 2.0f) : p.speed * 2.0f;
    }
    const bool impactAtEnd = p.impactAtEnd;

    // The motion script decides how many fly (Drunken Missiles: 5); only the
    // first carries the impact and the sound.
    uint32_t motionId = 0, count = 1;
    if (v.motionId != 0) {
        auto mIt = motions_.find(v.motionId);
        if (mIt != motions_.end()) { motionId = v.motionId; count = mIt->second.missileCount; }
    }
    std::uniform_real_distribution<float> unit01(0.0f, 1.0f);
    bool launched = false;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t instanceId =
            m2Renderer_->createInstance(modelId, launch, glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) {
            LOG_WARNING("SpellVisual: failed to create missile instance for visualId=", visualId);
            break;
        }
        m2Renderer_->markInstanceSpellVisual(instanceId);
        m2Renderer_->restartInstanceAnimation(instanceId);
        SpellMissile m;
        m.instanceId = instanceId;
        m.visualId = visualId;
        m.pos = m.shown = m.launch = launch;
        m.end = aim;
        m.endFeet = end;
        m.targetInstanceId = groundTarget ? 0u : targetInstanceId;
        m.targetGuid = groundTarget ? 0u : p.targetGuid;
        m.destAttachment = destAttachment;
        m.speed = speed;
        m.startDistance = distance;
        m.motionId = motionId;
        // MissilePathType 2 is the lobbed path - mortars, bombs, flares,
        // torches (69 visuals). Those with a script already arc in it; the
        // rest get the stock "Parabola" (13) shape.
        m.arc = (motionId == 0 && v.pathType == 2);
        m.missileIndex = index;
        m.missileCount = count;
        for (float& r : m.rand) r = unit01(rng_);
        m.impactAtEnd = impactAtEnd && index == 0;
        m.groundTarget = groundTarget;
        if (index == 0 && v.missileSound != 0) {
            // MissileFireLoop and its kind loop for the flight; a one-shot
            // whoosh just plays from the launch.
            m.soundHandle = playSoundEntry(v.missileSound, launch, /*keepLoop=*/true,
                                           &m.soundLoops);
        }
        spellMissiles_.push_back(m);
        placeSpellMissile(spellMissiles_.back(), dir);
        launched = true;
    }
    LOG_DEBUG("SpellVisual: missile visualId=", visualId, " model=", v.missilePath,
              " distance=", distance, " speed=", speed, " eta=", distance / speed, "s",
              " motion=", motionId, " x", count, groundTarget ? " ground" : "");
    return launched;
}

// A weapon thrown as the missile (MissileModel -1 main hand, -2 off hand):
// a copy of the caster's weapon model leaves its hand and spins to the target
// at the spell's speed, and the impact plays where it lands.
bool SpellVisualSystem::launchWeaponMissile(const PendingMissile& p) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    auto vIt = visuals_.find(p.visualId);
    if (!cr || p.casterInstanceId == 0 || vIt == visuals_.end()) return false;
    const VisualRow& v = vIt->second;
    const uint32_t hand = v.weaponMissile == -2 ? 2u : 1u;
    glm::mat4 xf;
    glm::vec3 start = p.start + glm::vec3(0.0f, 0.0f, 1.0f);
    if (cr->getAttachmentTransform(p.casterInstanceId, hand, xf)) start = glm::vec3(xf[3]);
    const uint32_t copy = cr->createWeaponCopy(p.casterInstanceId, hand, start);
    if (copy == 0) return false;
    glm::vec3 aim = p.end;
    if (!p.groundTarget) {
        attachmentTransform(v.missileDestination, p.targetInstanceId, p.end,
                            kSlotAttach[SLOT_CHEST].fallbackHeight, xf);
        aim = glm::vec3(xf[3]);
    }
    const float distance = glm::length(aim - start);
    const float speed = std::max(p.speed, 1.0f);
    float duration = distance / speed - p.waited;
    duration = std::max(duration, distance / (speed * 2.0f));
    const glm::vec3 d = aim - start;
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(d.y, d.x);
    PhysicalProjectile proj;
    proj.instanceId = copy;
    proj.start = start;
    proj.end = aim;
    proj.rotation = rotation;
    proj.duration = std::max(duration, 0.05f);
    proj.spin = true;
    proj.impactVisual = p.impactAtEnd ? p.visualId : 0;
    proj.targetInstanceId = p.groundTarget ? 0u : p.targetInstanceId;
    proj.targetGuid = p.groundTarget ? 0u : p.targetGuid;
    proj.destAttachment = v.missileDestination;
    proj.endFeet = p.end;
    physicalProjectiles_.push_back(proj);
    if (v.missileSound != 0) playSoundEntry(v.missileSound, start, /*keepLoop=*/false);
    return true;
}

// The missile models point down +X. Turn it to face along its path, with the
// nose pitched up or down to follow a target above or below the caster, and
// add the motion script's own spin (modelPitch/Yaw/Roll, degrees) and scale.
void SpellVisualSystem::placeSpellMissile(const SpellMissile& missile, const glm::vec3& dir,
                                          const MotionOut* motion) {
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(dir.y, dir.x);
    rotation.y = -std::asin(std::clamp(dir.z, -1.0f, 1.0f));
    float scale = 1.0f;
    if (motion) {
        rotation.z += motion->modelYaw * kDegToRad;
        rotation.y += motion->modelPitch * kDegToRad;
        rotation.x += motion->modelRoll * kDegToRad;
        if (std::isfinite(motion->scale) && motion->scale > 0.01f)
            scale = std::min(motion->scale, 20.0f);
    }
    m2Renderer_->setInstanceTransform(missile.instanceId,
                                      placementModelMatrix(missile.shown, rotation, scale));
}

// ---------------------------------------------------------------------------
// Missile motion scripts (SpellMissileMotion.dbc)
// ---------------------------------------------------------------------------
// Each row is a few lines of Lua the stock client runs every frame for every
// missile: it reads progress (0..1 of the way), time (seconds flown),
// startDistance, missileIndex/missileCount, rand1-3 (fixed per missile),
// distanceToFirePos/distanceToImpactPos (yards flown and left) and
// totalDistance, and writes offsets off the straight line - transAngle
// (degrees round the line, 0 = up) with transMag (yards), transUp/Right/
// Front - spins modelPitch/Yaw/Roll (degrees), scale, and speedScalar or
// speedAbs. sin and cos take degrees. All 204 rows use nothing beyond that,
// if/elseif and math.fmod; they run here in a sandboxed Lua 5.1 state that
// has only the math library.
namespace {
int luaSinDeg(lua_State* L) {
    lua_pushnumber(L, std::sin(luaL_checknumber(L, 1) * (glm::pi<double>() / 180.0)));
    return 1;
}
int luaCosDeg(lua_State* L) {
    lua_pushnumber(L, std::cos(luaL_checknumber(L, 1) * (glm::pi<double>() / 180.0)));
    return 1;
}
int luaTanDeg(lua_State* L) {
    lua_pushnumber(L, std::tan(luaL_checknumber(L, 1) * (glm::pi<double>() / 180.0)));
    return 1;
}
void motionHook(lua_State* L, lua_Debug*) {
    luaL_error(L, "missile motion script ran too long");
}
constexpr const char* kMotionOutputs[] = {
    "transAngle", "transMag", "transUp", "transRight", "transFront",
    "modelPitch", "modelYaw", "modelRoll", "scale", "speedScalar", "speedscalar", "speedAbs",
};
}  // namespace

bool SpellVisualSystem::compileMotion(MotionRow& row, uint32_t motionId) {
    if (!motionLua_ && !motionLuaFailed_) {
        motionLua_ = luaL_newstate();
        if (!motionLua_) {
            motionLuaFailed_ = true;
        } else {
            lua_State* L = motionLua_;
            lua_pushcfunction(L, luaopen_math);
            lua_pushstring(L, LUA_MATHLIBNAME);
            lua_call(L, 1, 0);
            lua_register(L, "sin", luaSinDeg);
            lua_register(L, "cos", luaCosDeg);
            lua_register(L, "tan", luaTanDeg);
            lua_getglobal(L, LUA_MATHLIBNAME);
            for (const char* fn : {"abs", "sqrt", "floor", "ceil", "min", "max", "fmod", "random"}) {
                lua_getfield(L, -1, fn);
                lua_setglobal(L, fn);
            }
            lua_pop(L, 1);
        }
    }
    if (!motionLua_) { row.ref = -1; return false; }
    lua_State* L = motionLua_;
    const std::string chunk = "motion " + std::to_string(motionId);
    if (luaL_loadbuffer(L, row.script.data(), row.script.size(), chunk.c_str()) != 0) {
        LOG_WARNING("SpellVisual: missile motion ", motionId, " (", row.name,
                    ") does not compile: ", lua_tostring(L, -1));
        lua_pop(L, 1);
        row.ref = -1;
        return false;
    }
    row.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return true;
}

bool SpellVisualSystem::evalMotion(SpellMissile& m, float progress, MotionOut& out) {
    out = MotionOut{};
    out.speedAbs = m.speed;
    if (m.motionId == 0) {
        if (!m.arc) return false;
        // The stock "Parabola" (13): up by 15% of the distance at the middle.
        const float t = progress * 2.0f - 1.0f;
        out.transMag = (1.0f - t * t) * m.startDistance * 0.15f;
        return true;
    }
    auto it = motions_.find(m.motionId);
    if (it == motions_.end()) return false;
    MotionRow& row = it->second;
    if (row.ref == -2) compileMotion(row, m.motionId);
    if (row.ref < 0 || !motionLua_) return false;
    lua_State* L = motionLua_;

    const float remaining = std::max(glm::length(m.end - m.pos), 0.0f);
    auto setNum = [L](const char* name, double v) {
        lua_pushnumber(L, v);
        lua_setglobal(L, name);
    };
    setNum("progress", progress);
    setNum("time", m.elapsed);
    setNum("startDistance", m.startDistance);
    setNum("totalDistance", m.startDistance);
    setNum("missileIndex", m.missileIndex);
    setNum("missileCount", m.missileCount);
    setNum("rand1", m.rand[0]);
    setNum("rand2", m.rand[1]);
    setNum("rand3", m.rand[2]);
    setNum("distanceToFirePos", m.travelled);
    setNum("distanceToImpactPos", remaining);
    setNum("distanceFromImpactPos", remaining);
    for (const char* name : kMotionOutputs) setNum(name, 0.0);
    setNum("scale", 1.0);
    setNum("speedScalar", 1.0);
    setNum("speedscalar", 1.0);
    setNum("speedAbs", m.speed);

    lua_rawgeti(L, LUA_REGISTRYINDEX, row.ref);
    lua_sethook(L, motionHook, LUA_MASKCOUNT, 100000);  // also resets the count
    if (lua_pcall(L, 0, 0, 0) != 0) {
        LOG_WARNING("SpellVisual: missile motion ", m.motionId, " (", row.name,
                    ") failed: ", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_sethook(L, nullptr, 0, 0);
        luaL_unref(L, LUA_REGISTRYINDEX, row.ref);
        row.ref = -1;  // once is enough to know
        return false;
    }
    lua_sethook(L, nullptr, 0, 0);
    auto getNum = [L](const char* name, float fallback) -> float {
        lua_getglobal(L, name);
        const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1)) : fallback;
        lua_pop(L, 1);
        return std::isfinite(v) ? v : fallback;
    };
    out.transAngle = getNum("transAngle", 0.0f);
    out.transMag   = getNum("transMag", 0.0f);
    out.transUp    = getNum("transUp", 0.0f);
    out.transRight = getNum("transRight", 0.0f);
    out.transFront = getNum("transFront", 0.0f);
    out.modelPitch = getNum("modelPitch", 0.0f);
    out.modelYaw   = getNum("modelYaw", 0.0f);
    out.modelRoll  = getNum("modelRoll", 0.0f);
    out.scale      = getNum("scale", 1.0f);
    // "speedscalar" is one row's spelling (Soul Vortex 7 Missiles - Resyncced).
    const float scalarA = getNum("speedScalar", 1.0f);
    const float scalarB = getNum("speedscalar", 1.0f);
    out.speedScalar = scalarA != 1.0f ? scalarA : scalarB;
    out.speedAbs = getNum("speedAbs", m.speed);
    return true;
}

float SpellVisualSystem::groundHeightAt(const glm::vec3& pos) const {
    float best = -std::numeric_limits<float>::infinity();
    if (!renderer_) return best;
    if (auto* terrain = renderer_->getTerrainManager())
        if (auto h = terrain->getHeightAt(pos.x, pos.y)) best = *h;
    // A floor under it inside a building or on a bridge wins when it is
    // higher than the terrain and not above the missile.
    if (auto* wmo = renderer_->getWMORenderer())
        if (auto h = wmo->getFloorHeight(pos.x, pos.y, pos.z + 2.0f); h && *h > best) best = *h;
    return best;
}

void SpellVisualSystem::playPhysicalProjectile(const std::string& modelPath,
                                                const std::string& texturePath,
                                                const glm::vec3& start,
                                                const glm::vec3& end,
                                                float duration,
                                                bool spin) {
    if (!renderer_ || modelPath.empty()) return;
    auto* characterRenderer = renderer_->getCharacterRenderer();
    if (!characterRenderer) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;

    const std::string cacheKey = modelPath + "|" + texturePath;
    uint32_t modelId = 0;
    auto cached = projectileModelIds_.find(cacheKey);
    if (cached != projectileModelIds_.end()) {
        modelId = cached->second;
    } else {
        if (nextProjectileModelId_ >= 999000) return;
        modelId = nextProjectileModelId_++;
        projectileModelIds_[cacheKey] = modelId;
    }

    if (!characterRenderer->getModelData(modelId)) {
        auto bytes = cachedAssetManager_->readFile(modelPath);
        if (bytes.empty()) return;
        pipeline::M2Model model = pipeline::M2Loader::load(bytes);
        if (model.name.empty()) model.name = modelPath;
        if (model.version >= 264) {
            const std::string skinPath = pipeline::skinPathForM2(modelPath);
            auto skin = cachedAssetManager_->readFile(skinPath);
            if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
        }
        if (model.vertices.empty() || !characterRenderer->loadModel(model, modelId)) return;
        if (!texturePath.empty()) {
            if (auto* texture = characterRenderer->loadTexture(texturePath))
                characterRenderer->setModelTexture(modelId, 0, texture);
        }
    }

    const glm::vec3 delta = end - start;
    const float horizontal = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(delta.y, delta.x);
    rotation.y = -std::atan2(delta.z, std::max(horizontal, 0.001f));

    const uint32_t instanceId = characterRenderer->createInstance(modelId, start, rotation, 1.0f);
    if (instanceId == 0) return;
    physicalProjectiles_.push_back({.instanceId = instanceId, .start = start, .end = end, .rotation = rotation, .elapsed = 0.0f,
                                    .duration = std::max(duration, 0.05f), .spin = spin});
}

// ---------------------------------------------------------------------------
// Ending effects
// ---------------------------------------------------------------------------
// An effect at the end of its life used to vanish on the frame, taking every
// spark and wisp in the air with it. Now it stops making new ones and stays
// until those have finished: a model with a Decay sequence (159) plays it
// first, and the mesh fades out over a quarter second. Capped, so a model
// with long-lived particles does not hang about.
void SpellVisualSystem::retireVisual(SpellVisualInstance& sv) {
    if (sv.retiring) return;
    sv.retiring = true;
    sv.linger = 0.0f;
    // The kit's procedures end with it: its tint, its lasting chain effect,
    // its area's shards.
    if (sv.procToken != 0) {
        endProcs(sv.procToken);
        sv.procToken = 0;
    }
    if (sv.instanceId == 0) {
        if (sv.soundHandle) audio::AudioEngine::instance().fadeOutSound(sv.soundHandle, 300.0f);
        sv.soundHandle = 0;
        sv.lingerCap = 0.0f;  // gone next frame
        return;
    }
    if (sv.hasDecay && m2Renderer_) {
        m2Renderer_->setInstanceAnimationHeld(sv.instanceId, kAnimDecay, /*skipToEnd=*/false);
        sv.decayDuration = std::clamp(m2Renderer_->getInstanceAnimDuration(sv.instanceId) / 1000.0f,
                                      0.0f, 2.0f);
    } else {
        sv.decayDuration = 0.0f;
    }
    sv.lingerCap = std::max(SPELL_LINGER_CAP, sv.decayDuration + 0.5f);
}

void SpellVisualSystem::removeVisualNow(SpellVisualInstance& sv) {
    if (sv.instanceId && m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    if (sv.soundHandle) audio::AudioEngine::instance().stopSound(sv.soundHandle);
    sv.instanceId = 0;
    sv.soundHandle = 0;
}

void SpellVisualSystem::retireMissileModel(uint32_t instanceId) {
    if (instanceId == 0) return;
    SpellVisualInstance sv;
    sv.instanceId = instanceId;
    sv.duration = 0.0f;
    activeSpellVisuals_.push_back(sv);
    retireVisual(activeSpellVisuals_.back());
}

void SpellVisualSystem::update(float deltaTime) {
    refreshUnitStates(deltaTime);
    // The kits' character procedures: chain effects' bolts, area shards and
    // the tints on units.
    if (m2Renderer_) {
        std::vector<StripOut> strips;
        updateChainEffects(deltaTime, strips);
        updateSwingTrails(deltaTime, strips);
        if (!strips.empty() || hadStrips_) submitStrips(strips);
        hadStrips_ = !strips.empty();
    }
    updateShards(deltaTime);
    updateUnitTints(deltaTime);
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty() && spellMissiles_.empty() &&
        pendingMissiles_.empty())
        return;

    // Missiles home on the target's attachment at the spell's speed, so one
    // fired at a running target still lands on it. Arrivals are collected
    // first: the impact spawns into activeSpellVisuals_ on the target.
    struct Arrival {
        uint32_t visualId; glm::vec3 feet; uint32_t targetInstanceId; bool ground;
        uint64_t targetGuid;
    };
    std::vector<Arrival> arrivals;
    std::vector<uint32_t> landed;
    auto& audioEngine = audio::AudioEngine::instance();
    // Missiles still in the caster's hand: each goes when its release comes.
    for (size_t i = 0; i < pendingMissiles_.size(); ) {
        PendingMissile& p = pendingMissiles_[i];
        p.waited += deltaTime;
        if (p.waited < p.delay) { ++i; continue; }
        const PendingMissile ready = p;
        pendingMissiles_[i] = pendingMissiles_.back();
        pendingMissiles_.pop_back();
        launchSpellMissile(ready);
    }
    for (auto it = spellMissiles_.begin(); it != spellMissiles_.end(); ) {
        SpellMissile& m = *it;
        m.elapsed += deltaTime;
        if (m.targetInstanceId != 0) {
            glm::mat4 xf;
            if (attachmentTransform(m.destAttachment, m.targetInstanceId, m.endFeet,
                                    kSlotAttach[SLOT_CHEST].fallbackHeight, xf))
                m.end = glm::vec3(xf[3]);
        }
        const glm::vec3 toEnd = m.end - m.pos;
        const float remaining = glm::length(toEnd);
        const float step = std::clamp(m.speed * m.speedFactor, 0.5f, 500.0f) * deltaTime;
        // A missile still flying after this long has lost its target to a
        // teleport or a despawn; the server settled the hit long ago.
        if (remaining <= step || remaining < 1e-3f || m.elapsed >= SPELL_MISSILE_MAX_DURATION) {
            // The stock impact (0x703410) puts the missile on the impact
            // point before it fades over 0.15 s, so its trail ends there.
            if (remaining > 1e-3f && m.elapsed < SPELL_MISSILE_MAX_DURATION) {
                const glm::vec3 dirIn = toEnd / remaining;
                m.pos = m.shown = m.end;
                placeSpellMissile(m, dirIn);
            }
            if (m.soundHandle && m.soundLoops) audioEngine.fadeOutSound(m.soundHandle, 150.0f);
            landed.push_back(m.instanceId);
            if (m.impactAtEnd)
                arrivals.push_back({m.visualId, m.groundTarget ? m.end : m.endFeet,
                                    m.targetInstanceId, m.groundTarget, m.targetGuid});
            it = spellMissiles_.erase(it);
            continue;
        }
        const glm::vec3 dir = toEnd / remaining;
        m.pos += dir * step;
        m.travelled += step;

        // MissileFollowGround (flags bit 0 - Incinerate's fire, Typhoon's
        // wave, Crypt Scarabs): run along the ground at its height, dropping
        // to it at DropSpeed, until Approach yards out, where the homing
        // takes it up to the target.
        const VisualRow* v = nullptr;
        if (auto vIt = visuals_.find(m.visualId); vIt != visuals_.end()) v = &vIt->second;
        if (v && (v->followGroundFlags & 1u) && remaining - step > v->followGroundApproach) {
            const float ground = groundHeightAt(m.pos);
            if (std::isfinite(ground)) {
                const float want = ground + v->followGroundHeight;
                const float drop = v->followGroundDropSpeed > 0.0f
                    ? v->followGroundDropSpeed * deltaTime : std::abs(m.pos.z - want);
                if (m.pos.z > want) m.pos.z = std::max(want, m.pos.z - drop);
                else m.pos.z = want;  // never through the ground
            }
        }

        // The path's shape: offsets off the homing line from the motion
        // script (or the plain lob), in the frame of the line - front along
        // it, up as near the world's up as the line allows, right across.
        const float left = glm::length(m.end - m.pos);
        const float progress = std::clamp(m.travelled / std::max(m.travelled + left, 1e-3f), 0.0f, 1.0f);
        MotionOut motion;
        const bool shaped = evalMotion(m, progress, motion);
        glm::vec3 shown = m.pos;
        if (shaped) {
            glm::vec3 front = m.end - m.pos;
            const float fl = glm::length(front);
            front = fl > 1e-4f ? front / fl : dir;
            glm::vec3 up(0.0f, 0.0f, 1.0f);
            up = up - front * glm::dot(up, front);
            const float ul = glm::length(up);
            up = ul > 1e-3f ? up / ul : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 right = glm::cross(front, up);
            const float a = motion.transAngle * kDegToRad;
            shown += (std::cos(a) * up + std::sin(a) * right) * motion.transMag;
            shown += up * motion.transUp + right * motion.transRight + front * motion.transFront;
            if (!std::isfinite(shown.x) || !std::isfinite(shown.y) || !std::isfinite(shown.z))
                shown = m.pos;
            // speedAbs is a speed in yards a second, speedScalar a factor.
            const float absFactor = m.speed > 0.0f ? motion.speedAbs / m.speed : 1.0f;
            m.speedFactor = std::clamp(absFactor * motion.speedScalar, 0.01f, 100.0f);
        }
        glm::vec3 facing = shown - m.shown;
        const float facingLen = glm::length(facing);
        facing = facingLen > 1e-4f ? facing / facingLen : dir;
        m.shown = shown;
        placeSpellMissile(m, facing, shaped ? &motion : nullptr);
        if (m.soundHandle && m.soundLoops) audioEngine.setSoundPosition(m.soundHandle, m.shown);
        ++it;
    }
    for (uint32_t id : landed) retireMissileModel(id);
    for (const auto& a : arrivals) {
        if (a.ground) {
            auto vIt = visuals_.find(a.visualId);
            if (vIt != visuals_.end()) {
                KitSpawn s;
                s.feetPos = a.feet;
                s.atPoint = true;
                spawnKit(vIt->second.impactAreaKit, s);
            }
        } else {
            playSpellVisual(a.visualId, a.feet, /*useImpactKit=*/true, a.targetInstanceId,
                            /*targetIsCaster=*/false, a.targetGuid);
            if (missileImpactCallback_ && a.targetGuid != 0)
                missileImpactCallback_(a.targetGuid, a.visualId);
        }
    }

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    for (size_t i = 0; i < activeSpellVisuals_.size(); ) {
        SpellVisualInstance& sv = activeSpellVisuals_[i];
        sv.elapsed += deltaTime;
        if (!sv.retiring && sv.elapsed >= sv.duration) retireVisual(sv);

        bool done = false;
        if (sv.retiring) {
            sv.linger += deltaTime;
            if (sv.instanceId == 0) {
                done = sv.linger >= sv.lingerCap;
            } else {
                // Decay first (it winds the emitters down itself), then no
                // new particles at all; the mesh fades meanwhile.
                if (!sv.emissionStopped && sv.linger >= sv.decayDuration) {
                    m2Renderer_->stopInstanceEmission(sv.instanceId);
                    sv.emissionStopped = true;
                }
                const float fadeFrom = sv.decayDuration;
                if (sv.linger > fadeFrom)
                    m2Renderer_->setInstanceFade(
                        sv.instanceId, 1.0f - std::min((sv.linger - fadeFrom) / SPELL_MESH_FADE, 1.0f));
                done = sv.linger >= sv.lingerCap ||
                       (sv.emissionStopped && sv.linger >= fadeFrom + SPELL_MESH_FADE &&
                        !m2Renderer_->instanceHasLiveParticles(sv.instanceId));
            }
        } else if (sv.instanceId != 0 && sv.hasHold && !sv.noHold && !sv.holding &&
                   sv.birthDuration > 0.0f && sv.elapsed >= sv.birthDuration &&
                   sv.duration > sv.birthDuration + 0.1f) {
            // Born; now hold for as long as the kit lasts.
            m2Renderer_->setInstanceAnimation(sv.instanceId, kAnimHold, /*loop=*/true);
            sv.holding = true;
        }
        if (done) {
            removeVisualNow(sv);
            activeSpellVisuals_[i] = std::move(activeSpellVisuals_.back());
            activeSpellVisuals_.pop_back();
            continue;
        }
        // Ride the unit's attachment point - position, facing and scale -
        // the way the stock client parents kit models to the unit.
        if (sv.attachInstanceId != 0 && charRenderer) {
            glm::mat4 xf;
            const bool rides = sv.rawAttachment >= 0
                ? attachmentTransform(sv.rawAttachment, sv.attachInstanceId, glm::vec3(0.0f), 0.0f, xf)
                : slotTransform(sv.slot, sv.attachInstanceId, glm::vec3(0.0f), xf);
            if (sv.rawAttachment >= 0) xf = xf * sv.attachLocal;
            if (rides) {
                if (sv.instanceId) m2Renderer_->setInstanceTransform(sv.instanceId, xf);
                if (sv.soundHandle) audioEngine.setSoundPosition(sv.soundHandle, glm::vec3(xf[3]));
                sv.effectPos = glm::vec3(xf[3]);
            }
        }
        // The model's own $SHK camera shakes, the same way (0x704C30).
        if (sv.instanceId != 0 && !sv.retiring) {
            auto shk = modelEffectShakes_.find(sv.modelId);
            if (shk != modelEffectShakes_.end()) {
                const float nowMs = sv.elapsed * 1000.0f;
                while (sv.nextEffectShake < shk->second.size() &&
                       static_cast<float>(shk->second[sv.nextEffectShake].timeMs) <= nowMs) {
                    playCameraShake(shk->second[sv.nextEffectShake].soundEntryId, sv.effectPos);
                    ++sv.nextEffectShake;
                }
            }
        }
        // The model's own $SND events, each once as the birth sequence passes
        // its time. Not after the model starts to end: Decay is a different
        // sequence with no events of its own.
        if (sv.instanceId != 0 && !sv.retiring) {
            auto snd = modelEffectSounds_.find(sv.modelId);
            if (snd != modelEffectSounds_.end()) {
                const float nowMs = sv.elapsed * 1000.0f;
                const auto& list = snd->second;
                while (sv.nextEffectSound < list.size() &&
                       static_cast<float>(list[sv.nextEffectSound].timeMs) <= nowMs) {
                    playSoundEntry(list[sv.nextEffectSound].soundEntryId, sv.effectPos,
                                   /*keepLoop=*/false, nullptr,
                                   sv.attachInstanceId != 0 && renderer_ &&
                                       sv.attachInstanceId == renderer_->getCharacterInstanceId());
                    ++sv.nextEffectSound;
                }
            }
        }
        ++i;
    }


    if (charRenderer) {
        for (auto it = physicalProjectiles_.begin(); it != physicalProjectiles_.end(); ) {
            it->elapsed += deltaTime;
            // A thrown weapon follows its target.
            if (it->targetInstanceId != 0) {
                glm::mat4 xf;
                if (attachmentTransform(it->destAttachment, it->targetInstanceId, it->endFeet,
                                        kSlotAttach[SLOT_CHEST].fallbackHeight, xf))
                    it->end = glm::vec3(xf[3]);
            }
            const float t = std::min(it->elapsed / it->duration, 1.0f);
            charRenderer->setInstancePosition(
                it->instanceId, glm::mix(it->start, it->end, t));
            if (it->spin) {
                glm::vec3 rotation = it->rotation;
                rotation.x += t * glm::two_pi<float>() * 2.0f;
                charRenderer->setInstanceRotation(it->instanceId, rotation);
            }
            if (t >= 1.0f) {
                charRenderer->removeInstance(it->instanceId);
                if (it->impactVisual != 0) {
                    const uint32_t visual = it->impactVisual;
                    const uint64_t target = it->targetGuid;
                    const uint32_t targetInst = it->targetInstanceId;
                    const glm::vec3 feet = it->endFeet;
                    it = physicalProjectiles_.erase(it);
                    playSpellVisual(visual, feet, /*useImpactKit=*/true, targetInst, false, target);
                    if (missileImpactCallback_ && target != 0) missileImpactCallback_(target, visual);
                    continue;
                }
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void SpellVisualSystem::cancelPrecastVisuals(uint64_t casterGuid) {
    if (!m2Renderer_ || casterGuid == 0) return;
    // The build-up winds down rather than blinking out (see retireVisual).
    for (auto& sv : activeSpellVisuals_)
        if (sv.isPrecast && sv.ownerGuid == casterGuid && !sv.retiring) retireVisual(sv);
}

// ---------------------------------------------------------------------------
// Aura state visuals (SpellVisual.StateKit)
// ---------------------------------------------------------------------------
void SpellVisualSystem::setUnitResolvers(UnitInstanceResolver instanceOf,
                                         UnitPositionResolver feetOf) {
    unitInstanceOf_ = std::move(instanceOf);
    unitFeetOf_ = std::move(feetOf);
}

void SpellVisualSystem::spawnUnitState(uint64_t guid, UnitStateRecord& rec) {
    auto vIt = visuals_.find(rec.visualId);
    if (vIt == visuals_.end()) return;
    // Only on a unit that is drawn: a state kit rides the unit's attachments
    // and has nothing to stand on otherwise.
    const uint32_t inst = unitInstanceOf_ ? unitInstanceOf_(guid) : 0u;
    if (inst == 0) return;
    if (vIt->second.stateKit == 0) {  // only a done kit: nothing to show yet
        rec.attachInstanceId = inst;
        rec.spawned = true;
        return;
    }
    glm::vec3 feet(0.0f);
    if (!unitFeetOf_ || !unitFeetOf_(guid, feet)) return;
    KitSpawn s;
    s.feetPos = feet;
    s.attachInstanceId = inst;
    s.lifetime = std::numeric_limits<float>::infinity();
    s.stateGuid = guid;
    s.stateVisual = rec.visualId;
    s.unitGuid = guid;
    s.visualId = rec.visualId;
    spawnKit(vIt->second.stateKit, s);
    rec.attachInstanceId = inst;
    rec.spawned = true;
    rec.unresolvedTime = 0.0f;
}

void SpellVisualSystem::retireUnitState(uint64_t guid, uint32_t visualId) {
    for (auto& sv : activeSpellVisuals_)
        if (sv.stateGuid == guid && sv.stateVisual == visualId && !sv.retiring) retireVisual(sv);
}

void SpellVisualSystem::syncUnitStateVisuals(uint64_t guid, const std::vector<uint32_t>& visualIds) {
    if (guid == 0 || !ensureLoaded()) return;
    // Only the visuals that have a state kit (or a done kit) matter here.
    std::vector<uint32_t> wanted;
    for (uint32_t vid : visualIds) {
        if (vid == 0) continue;
        auto vIt = visuals_.find(vid);
        if (vIt == visuals_.end()) continue;
        if (!vIt->second.stateKit && !vIt->second.stateDoneKit) continue;
        if (std::find(wanted.begin(), wanted.end(), vid) == wanted.end()) wanted.push_back(vid);
    }
    auto& recs = unitStates_[guid];
    // Gone: the state ends (Decay, then its particles), and the done kit
    // plays - Ice Block shattering, a shield breaking.
    for (auto it = recs.begin(); it != recs.end(); ) {
        if (std::find(wanted.begin(), wanted.end(), it->visualId) != wanted.end()) { ++it; continue; }
        retireUnitState(guid, it->visualId);
        auto vIt = visuals_.find(it->visualId);
        if (vIt != visuals_.end() && vIt->second.stateDoneKit) {
            glm::vec3 feet(0.0f);
            if (unitFeetOf_ && unitFeetOf_(guid, feet)) {
                KitSpawn s;
                s.feetPos = feet;
                s.attachInstanceId = unitInstanceOf_ ? unitInstanceOf_(guid) : 0u;
                s.unitGuid = guid;
                s.visualId = it->visualId;
                spawnKit(vIt->second.stateDoneKit, s);
            }
        }
        it = recs.erase(it);
    }
    // New: the state kit loops on the unit from now on.
    for (uint32_t vid : wanted) {
        bool have = false;
        for (const auto& r : recs) if (r.visualId == vid) { have = true; break; }
        if (have) continue;
        UnitStateRecord rec;
        rec.visualId = vid;
        spawnUnitState(guid, rec);
        recs.push_back(rec);
    }
    if (recs.empty()) unitStates_.erase(guid);
}

void SpellVisualSystem::clearAllUnitStateVisuals() {
    for (auto& sv : activeSpellVisuals_)
        if (sv.stateGuid != 0 && !sv.retiring) retireVisual(sv);
    unitStates_.clear();
}

// A unit's model comes and goes - it walks out of range and back, it is
// re-made on a display change, an aura arrives before the unit is drawn.
// Twice a second each state is checked against the unit's current instance
// and moved over to it; a unit unseen for a minute is forgotten.
void SpellVisualSystem::refreshUnitStates(float deltaTime) {
    if (unitStates_.empty() || !unitInstanceOf_) return;
    stateRecheckTimer_ += deltaTime;
    if (stateRecheckTimer_ < 0.5f) return;
    const float step = stateRecheckTimer_;
    stateRecheckTimer_ = 0.0f;
    for (auto it = unitStates_.begin(); it != unitStates_.end(); ) {
        const uint64_t guid = it->first;
        const uint32_t inst = unitInstanceOf_(guid);
        bool anyResolved = false;
        for (auto& rec : it->second) {
            if (rec.spawned && rec.attachInstanceId == inst && inst != 0) { anyResolved = true; continue; }
            if (rec.spawned) {
                retireUnitState(guid, rec.visualId);
                rec.spawned = false;
                rec.attachInstanceId = 0;
            }
            if (inst != 0) spawnUnitState(guid, rec);
            if (rec.spawned) anyResolved = true;
            else rec.unresolvedTime += step;
        }
        bool stale = !anyResolved;
        for (const auto& rec : it->second)
            if (rec.spawned || rec.unresolvedTime < 60.0f) { stale = false; break; }
        if (stale) it = unitStates_.erase(it);
        else ++it;
    }
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) removeVisualNow(sv);
    activeSpellVisuals_.clear();
    for (const auto& missile : spellMissiles_) {
        if (m2Renderer_) m2Renderer_->removeInstance(missile.instanceId);
        if (missile.soundHandle) audio::AudioEngine::instance().stopSound(missile.soundHandle);
    }
    spellMissiles_.clear();
    pendingMissiles_.clear();
    // Aura states stay known (the auras have not changed); they are put
    // back on their units as the units are drawn again.
    for (auto& [guid, recs] : unitStates_)
        for (auto& rec : recs) { rec.spawned = false; rec.attachInstanceId = 0; rec.unresolvedTime = 0.0f; }
    if (renderer_ && renderer_->getCharacterRenderer()) {
        for (const auto& projectile : physicalProjectiles_)
            renderer_->getCharacterRenderer()->removeInstance(projectile.instanceId);
    }
    physicalProjectiles_.clear();
    // The procedures went with their kits.
    chains_.clear();
    swingTrails_.clear();
    if (renderer_) {
        if (auto* cr = renderer_->getCharacterRenderer())
            for (const auto& f : frozenPoses_) cr->setInstanceAnimationFrozen(f.instanceId, false);
    }
    frozenPoses_.clear();
    unitTints_.clear();
    unitFlashes_.clear();
    shardEmitters_.clear();
    pendingShards_.clear();
    if (renderer_) {
        if (auto* cr = renderer_->getCharacterRenderer())
            for (uint32_t inst : tintedInstances_) cr->setInstanceDiffuseTint(inst, glm::vec3(1.0f));
    }
    tintedInstances_.clear();
    if (m2Renderer_) m2Renderer_->setEffectStrips({});
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}


// ---------------------------------------------------------------------------
// Character procedures (SpellVisualKit CharProc, Wow.exe 0x7265C0)
// ---------------------------------------------------------------------------
namespace {
// A CharParam colour: 0xRRGGBB held in a float (0x720DB0 reads r from bits
// 16-23).
glm::vec3 procColour(float packed) {
    const auto c = static_cast<uint32_t>(std::llround(std::max(packed, 0.0f)));
    return glm::vec3(static_cast<float>((c >> 16) & 0xFFu), static_cast<float>((c >> 8) & 0xFFu),
                     static_cast<float>(c & 0xFFu)) / 255.0f;
}
}  // namespace

uint32_t SpellVisualSystem::applyKitProcs(uint32_t kitId, const KitRow& kit, const KitSpawn& spawn) {
    (void)kitId;
    const uint32_t token = nextProcToken_++;
    if (nextProcToken_ == 0) nextProcToken_ = 1;
    bool used = false;
    for (const KitProc& proc : kit.procs) {
        switch (proc.type) {
            case 0:
            case 12:
                // A chain effect along the cast's targets, or to the channel
                // target (0x7FC950 -> 0x7FC5F0). ParamZero the row, ParamTwo
                // it lasts as long as the kit, ParamThree every hop starts at
                // the source.
                startChainEffect(static_cast<uint32_t>(std::lround(proc.p[0])), proc, spawn, token,
                                 kit.animId);
                used = true;
                break;
            case 1:
                // The unit's model colour (0x7178E0 drops the spell's older
                // one; the newest on the unit shows, 0x720DB0).
                if (spawn.attachInstanceId != 0) {
                    const uint32_t key = spawn.visualId;
                    unitTints_.erase(std::remove_if(unitTints_.begin(), unitTints_.end(),
                                                    [&](const UnitTint& t) {
                                                        return t.instanceId == spawn.attachInstanceId &&
                                                               key != 0 && t.key == key;
                                                    }),
                                     unitTints_.end());
                    unitTints_.push_back({spawn.attachInstanceId, token, key, procColour(proc.p[0])});
                    used = true;
                }
                break;
            case 13:
                // A flash: the colour for ParamOne seconds, then back to white
                // over ParamTwo (0x71A9A0). Not tied to the kit.
                if (spawn.attachInstanceId != 0) {
                    unitFlashes_.erase(std::remove_if(unitFlashes_.begin(), unitFlashes_.end(),
                                                      [&](const UnitFlash& f) {
                                                          return f.instanceId == spawn.attachInstanceId;
                                                      }),
                                       unitFlashes_.end());
                    unitFlashes_.push_back({spawn.attachInstanceId, procColour(proc.p[0]),
                                            proc.p[1] * 1000.0f, proc.p[2] * 1000.0f, 0.0f});
                }
                break;
            case 8:
                // A swing trail on each weapon in hand: the colour ParamZero
                // with ParamThree's alpha, for ParamTwo ms (0x7265C0 ->
                // 0x715BA0). Not tied to the kit.
                if (spawn.attachInstanceId != 0 && renderer_) {
                    auto* cr = renderer_->getCharacterRenderer();
                    const size_t n = cr ? cr->getWeaponAttachmentCount(spawn.attachInstanceId) : 0;
                    for (size_t w = 0; w < n && w < 2; ++w) {
                        // The swing restarts on a weapon already trailing (0x7E4FF0).
                        swingTrails_.erase(std::remove_if(swingTrails_.begin(), swingTrails_.end(),
                                                          [&](const SwingTrail& t) {
                                                              return t.instanceId == spawn.attachInstanceId &&
                                                                     t.weapon == w;
                                                          }),
                                           swingTrails_.end());
                        SwingTrail t;
                        t.instanceId = spawn.attachInstanceId;
                        t.weapon = w;
                        t.color = procColour(proc.p[0]);
                        t.alpha = std::clamp(std::round(proc.p[3]), 0.0f, 255.0f);
                        t.durationMs = std::max(proc.p[2], 0.0f);
                        if (t.durationMs > 0.0f && t.alpha > 0.0f) swingTrails_.push_back(std::move(t));
                    }
                }
                break;
            case 11:
                // The unit's pose held for as long as the kit lasts, at
                // ParamZero seconds into its animation when that is set
                // (0x6F80B0): Ice Block's frozen figure.
                if (spawn.attachInstanceId != 0 && renderer_) {
                    if (auto* cr = renderer_->getCharacterRenderer()) {
                        cr->setInstanceAnimationFrozen(spawn.attachInstanceId, true,
                                                       proc.p[0] > 0.0f ? proc.p[0] * 1000.0f : -1.0f);
                        frozenPoses_.push_back({spawn.attachInstanceId, token});
                        used = true;
                    }
                }
                break;
            case 9:
                // An area's falling shards (DynamicObject_C 0x704F60 ->
                // 0x7FBD70): the SpellVisualKitAreaModel row ParamZero, ParamOne
                // of them a second, anywhere in the area's radius.
                if (spawn.atPoint && spawn.areaRadius > 0.0f && proc.p[1] > 0.0f) {
                    loadAreaModels();
                    const auto idx = static_cast<size_t>(std::llround(proc.p[0])) & 0xFFu;
                    ShardEmitter e;
                    // A row past the table is Blizzard's (0x7FBD70).
                    e.model = idx < areaModels_.size() ? areaModels_[idx]
                            : (areaModels_.empty() ? std::string() : areaModels_.front());
                    e.center = spawn.feetPos;
                    e.radius = spawn.areaRadius;
                    e.rate = proc.p[1];
                    e.token = token;
                    if (!e.model.empty()) {
                        shardEmitters_.push_back(std::move(e));
                        used = true;
                    }
                }
                break;
            default:
                break;
        }
    }
    return used ? token : 0u;
}

void SpellVisualSystem::endProcs(uint32_t token) {
    if (token == 0) return;
    unitTints_.erase(std::remove_if(unitTints_.begin(), unitTints_.end(),
                                    [&](const UnitTint& t) { return t.token == token; }),
                     unitTints_.end());
    // A lasting chain goes with its kit; a timed one runs its hops out.
    chains_.erase(std::remove_if(chains_.begin(), chains_.end(),
                                 [&](const ChainEffect& c) { return c.token == token && c.persistent; }),
                  chains_.end());
    // A held pose lets go, unless another kit holds it too.
    for (auto it = frozenPoses_.begin(); it != frozenPoses_.end();) {
        if (it->token != token) { ++it; continue; }
        const uint32_t inst = it->instanceId;
        it = frozenPoses_.erase(it);
        const bool still = std::any_of(frozenPoses_.begin(), frozenPoses_.end(),
                                       [&](const FrozenPose& f) { return f.instanceId == inst; });
        if (!still && renderer_)
            if (auto* cr = renderer_->getCharacterRenderer()) cr->setInstanceAnimationFrozen(inst, false);
    }
    // The shards stop coming; those in the air land (0x7F9EE0).
    shardEmitters_.erase(std::remove_if(shardEmitters_.begin(), shardEmitters_.end(),
                                        [&](const ShardEmitter& e) { return e.token == token; }),
                         shardEmitters_.end());
}

void SpellVisualSystem::updateUnitTints(float deltaTime) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr) return;
    std::unordered_map<uint32_t, glm::vec3> want;
    // The newest tint on each unit (the head of the unit's list).
    for (const auto& t : unitTints_) want[t.instanceId] = t.color;
    // A flash overrides it while it lasts, then fades to white (0x71A9A0).
    for (auto it = unitFlashes_.begin(); it != unitFlashes_.end();) {
        it->ageMs += deltaTime * 1000.0f;
        if (it->ageMs < it->holdMs) {
            want[it->instanceId] = it->color;
        } else if (it->ageMs < it->holdMs + it->fadeMs) {
            const float k = 1.0f - (it->ageMs - it->holdMs) / it->fadeMs;
            want[it->instanceId] = glm::mix(glm::vec3(1.0f), it->color, k);
        } else {
            it = unitFlashes_.erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = tintedInstances_.begin(); it != tintedInstances_.end();) {
        if (!want.count(*it)) {
            cr->setInstanceDiffuseTint(*it, glm::vec3(1.0f));
            it = tintedInstances_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [inst, color] : want) {
        cr->setInstanceDiffuseTint(inst, color);
        if (tintedInstances_.insert(inst).second)
            LOG_DEBUG("SpellVisual: tint on instance ", inst, " (", color.r, ", ", color.g, ", ", color.b, ")");
    }
}

// ---------------------------------------------------------------------------
// Chain effects (SpellChainEffects, Wow.exe 0x7FC5F0 / 0x7FA4D0 / 0x7FAE90)
// ---------------------------------------------------------------------------
void SpellVisualSystem::loadChainEffects() {
    if (chainRowsLoaded_ || !cachedAssetManager_) return;
    chainRowsLoaded_ = true;
    auto dbc = cachedAssetManager_->loadDBC("SpellChainEffects.dbc");
    if (!dbc || !dbc->isLoaded()) return;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
        ChainEffectRow row;
        const bool ok = parseChainEffectRecord(
            dbc->getRecord(i), dbc->getRecordSize(), dbc->getFieldCount(),
            [&](uint32_t off) { return dbc->getStringByOffset(off); }, row);
        if (ok) chainRows_[row.id] = std::move(row);
    }
    LOG_INFO("SpellVisual: loaded ", chainRows_.size(), " chain effects");
}

const ChainEffectRow* SpellVisualSystem::chainEffectRow(uint32_t id) {
    if (!cachedAssetManager_) cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    loadChainEffects();
    auto it = chainRows_.find(id);
    return it != chainRows_.end() ? &it->second : nullptr;
}

void SpellVisualSystem::startChainEffect(uint32_t rowId, const KitProc& proc, const KitSpawn& spawn,
                                         uint32_t token, uint32_t castAnim) {
    const ChainEffectRow* first = chainEffectRow(rowId);
    if (!first || spawn.unitGuid == 0) return;
    // The hops' points: the unit the kit plays on, then whom it reaches - the
    // channel's target for a channel kit (UNIT_FIELD_CHANNEL_OBJECT, 0x7265C0),
    // else the cast's hit list in order (0x724F50 stores it backwards and
    // 0x7FA4D0 reads it backwards again). The unit itself is left out.
    std::vector<uint64_t> points{spawn.unitGuid};
    uint64_t channelCaster = 0;
    if (spawn.channelKit) {
        channelCaster = spawn.unitGuid;
        points.push_back(channelTargetOf_ ? channelTargetOf_(spawn.unitGuid) : 0);
    } else if (spawn.targets) {
        for (uint64_t g : *spawn.targets)
            if (g != 0 && g != spawn.unitGuid) points.push_back(g);
    }
    if (points.size() < 2) return;
    const bool persistent = proc.p[2] != 0.0f;
    const bool star = proc.p[3] != 0.0f;
    // The row and its combo rows each draw their own bolts along the hops.
    std::vector<const ChainEffectRow*> rows{first};
    for (uint32_t id : first->combo)
        if (const ChainEffectRow* r = chainEffectRow(id)) rows.push_back(r);
    for (const ChainEffectRow* row : rows) {
        ChainEffect c;
        c.row = row;
        c.points = points;
        c.channelCaster = channelCaster;
        c.visualId = spawn.visualId;
        c.castAnim = castAnim;
        c.persistent = persistent;
        c.token = token;
        // Hop i starts SegDelay + i x DelayBetweenEffects ms in and lasts
        // SegDuration (0x7FA4D0).
        float startMs = static_cast<float>(row->segDelay);
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            ChainHop hop;
            hop.from = static_cast<uint16_t>(star ? 0 : i);
            hop.to = static_cast<uint16_t>(i + 1);
            hop.startMs = startMs;
            hop.endMs = startMs + static_cast<float>(row->segDuration);
            c.endMs = std::max(c.endMs, hop.endMs);
            startMs += std::round(row->delayBetweenEffects);
            c.hops.push_back(std::move(hop));
        }
        LOG_DEBUG("SpellVisual: chain effect ", row->id, " from 0x", std::hex, points[0], std::dec,
                  " over ", points.size() - 1, " hop(s)", persistent ? " lasting" : "",
                  channelCaster ? " (channel)" : "");
        chains_.push_back(std::move(c));
    }
}

// Where a bolt leaves the source unit (0x7FAA40): the visual's
// MissileAttachment; else the cast animation's release point ($CSL); else
// the chest.
bool SpellVisualSystem::chainSourcePoint(const ChainEffect& c, uint64_t guid, glm::vec3& out) {
    const uint32_t inst = unitInstanceOf_ ? unitInstanceOf_(guid) : 0u;
    glm::vec3 feet(0.0f);
    const bool hasFeet = unitFeetOf_ && unitFeetOf_(guid, feet);
    if (inst == 0 && !hasFeet) return false;
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    auto vIt = visuals_.find(c.visualId);
    glm::mat4 xf;
    if (cr && inst != 0) {
        if (vIt != visuals_.end() && vIt->second.missileAttachment >= 0 &&
            cr->getAttachmentTransform(inst, static_cast<uint32_t>(vIt->second.missileAttachment), xf)) {
            out = glm::vec3(xf[3]);
            return true;
        }
        uint32_t ms = 0;
        for (uint32_t anim : {c.castAnim, 124u, 53u, 54u}) {
            if (anim != 0 && cr->getCastReleaseEvent(inst, anim, ms, &out)) return true;
        }
    }
    attachmentTransform(34, inst, feet, kSlotAttach[SLOT_CHEST].fallbackHeight, xf);
    out = glm::vec3(xf[3]);
    return true;
}

// Where a bolt reaches a unit (0x7FABF0): the visual's
// MissileDestinationAttachment, as a missile lands.
bool SpellVisualSystem::chainTargetPoint(const ChainEffect& c, uint64_t guid, glm::vec3& out) {
    const uint32_t inst = unitInstanceOf_ ? unitInstanceOf_(guid) : 0u;
    glm::vec3 feet(0.0f);
    const bool hasFeet = unitFeetOf_ && unitFeetOf_(guid, feet);
    if (inst == 0 && !hasFeet) return false;
    int32_t dest = 34;
    if (auto vIt = visuals_.find(c.visualId); vIt != visuals_.end()) dest = vIt->second.missileDestination;
    glm::mat4 xf;
    attachmentTransform(dest, inst, feet, kSlotAttach[SLOT_CHEST].fallbackHeight, xf);
    out = glm::vec3(xf[3]);
    return true;
}

void SpellVisualSystem::updateChainEffects(float deltaTime, std::vector<StripOut>& strips) {
    if (chains_.empty()) return;
    glm::vec3 camPos(0.0f);
    if (renderer_ && renderer_->getCamera()) camPos = renderer_->getCamera()->getPosition();
    for (auto it = chains_.begin(); it != chains_.end();) {
        ChainEffect& c = *it;
        c.ageMs += deltaTime * 1000.0f;
        if (!c.persistent && c.ageMs >= c.endMs) { it = chains_.erase(it); continue; }
        // A channel's target can arrive after the channel does, or change.
        if (c.channelCaster != 0 && channelTargetOf_ && c.points.size() >= 2) {
            if (const uint64_t t = channelTargetOf_(c.channelCaster)) c.points[1] = t;
        }
        for (auto& hop : c.hops) {
            const bool live = c.persistent || (c.ageMs >= hop.startMs && c.ageMs < hop.endMs);
            if (!live) {
                if (!c.persistent && c.ageMs >= hop.endMs) hop.bolt.reset();
                continue;
            }
            const uint64_t from = hop.from < c.points.size() ? c.points[hop.from] : 0;
            const uint64_t to = hop.to < c.points.size() ? c.points[hop.to] : 0;
            glm::vec3 a, b;
            // An end that cannot be placed hides the bolt but keeps it
            // (0x9A9770 clears its flag 2).
            if (from == 0 || to == 0) continue;
            const bool haveA = hop.from == 0 ? chainSourcePoint(c, from, a) : chainTargetPoint(c, from, a);
            if (!haveA || !chainTargetPoint(c, to, b)) continue;
            if (!hop.bolt) hop.bolt = std::make_unique<LightningBolt>(c.row, static_cast<uint32_t>(rng_()));
            hop.bolt->update(deltaTime, a, b);
            StripOut strip;
            strip.texture = c.row->texture;
            strip.gxBlend = c.row->blendMode;
            hop.bolt->build(camPos, strip.verts);
            if (strip.verts.size() >= 4) strips.push_back(std::move(strip));
        }
        ++it;
    }
}

void SpellVisualSystem::updateSwingTrails(float deltaTime, std::vector<StripOut>& strips) {
    auto* cr = renderer_ ? renderer_->getCharacterRenderer() : nullptr;
    if (!cr) { swingTrails_.clear(); return; }
    const float dtMs = deltaTime * 1000.0f;
    for (auto it = swingTrails_.begin(); it != swingTrails_.end();) {
        SwingTrail& t = *it;
        // 0x7E4CE0: while the swing lasts each frame lays the blade's ends;
        // the trail keeps alpha / step of them, step = the frame's ms / 300 x
        // alpha, so 300 ms of blade; past half its duration the alpha drops
        // a step a frame; under two edges it is gone.
        if (t.elapsedMs < t.durationMs) {
            glm::vec3 bottom, top;
            if (cr->getWeaponTrailPoints(t.instanceId, t.weapon, bottom, top))
                t.edges.emplace_back(bottom, top);
        }
        const float step = std::max(1.0f, std::round(dtMs * (1.0f / 300.0f) * t.alpha));
        const int keep = static_cast<int>(t.alpha / step);
        if (keep < 2 || (t.edges.empty() && t.elapsedMs >= t.durationMs)) { it = swingTrails_.erase(it); continue; }
        while (static_cast<int>(t.edges.size()) > keep) t.edges.erase(t.edges.begin());
        if (t.edges.size() >= 2) {
            // Oldest first, each edge a step dimmer than the one before.
            StripOut strip;
            strip.gxBlend = 2;
            float a = t.alpha;
            for (const auto& [b, tp] : t.edges) {
                a = std::max(a - step, 0.0f);
                const glm::vec4 c(t.color, a / 255.0f);
                strip.verts.push_back({b, c, glm::vec2(0.0f, 0.0f)});
                strip.verts.push_back({tp, c, glm::vec2(0.0f, 1.0f)});
            }
            strips.push_back(std::move(strip));
        }
        if (t.elapsedMs > t.durationMs * 0.5f) t.alpha = std::max(t.alpha - step, 0.0f);
        t.elapsedMs += dtMs;
        ++it;
    }
}

void SpellVisualSystem::submitStrips(std::vector<StripOut>& strips) {
    if (!m2Renderer_) return;
    std::vector<M2Renderer::EffectStrip> out;
    out.reserve(strips.size());
    for (auto& s : strips) {
        M2Renderer::EffectStrip e;
        e.texture = s.texture.empty() ? nullptr : m2Renderer_->loadEffectTexture(s.texture);
        e.gxBlend = s.gxBlend;
        e.verts.reserve(s.verts.size() * 9);
        for (const auto& v : s.verts) {
            e.verts.insert(e.verts.end(), {v.pos.x, v.pos.y, v.pos.z, v.color.r, v.color.g,
                                           v.color.b, v.color.a, v.uv.x, v.uv.y});
        }
        out.push_back(std::move(e));
    }
    m2Renderer_->setEffectStrips(std::move(out));
}

// ---------------------------------------------------------------------------
// Area shards (CharProc 9: the stock BlizzardObject, 0x7FB5B0 / 0x7FC220)
// ---------------------------------------------------------------------------
void SpellVisualSystem::loadAreaModels() {
    if (areaModelsLoaded_ || !cachedAssetManager_) return;
    areaModelsLoaded_ = true;
    // SpellVisualKitAreaModel.dbc: 0 ID, 1 ModelName, 2 Enum. The procedure
    // names a row by its place in the table (0x7FBD70 indexes the records).
    auto dbc = cachedAssetManager_->loadDBC("SpellVisualKitAreaModel.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 2) return;
    for (uint32_t i = 0; i < dbc->getRecordCount(); ++i)
        areaModels_.push_back(pipeline::modelPathToM2(dbc->getString(i, 1)));
}

void SpellVisualSystem::updateShards(float deltaTime) {
    // 0x7FC220: Rate a second, the clock capped at 1/15 s a frame; each shard
    // anywhere on the disc (a uniform fraction of the radius along a uniform
    // direction), put down on the ground under it, and shown 0-510 ms later.
    const float dt = std::min(deltaTime, 1.0f / 15.0f);
    std::uniform_real_distribution<float> unit01(0.0f, 1.0f);
    for (auto& e : shardEmitters_) {
        e.accum += dt * e.rate;
        while (e.accum > 1.0f) {
            e.accum -= 1.0f;
            const float r = unit01(rng_) * e.radius;
            const float a = unit01(rng_) * glm::two_pi<float>();
            glm::vec3 p = e.center + glm::vec3(std::cos(a) * r, std::sin(a) * r, 0.0f);
            const float ground = groundHeightAt(p + glm::vec3(0.0f, 0.0f, 16.6667f));
            if (std::isfinite(ground) && ground > e.center.z - 33.3333f && ground < e.center.z + 16.6667f)
                p.z = ground;
            const float delay = static_cast<float>(static_cast<int>(unit01(rng_) * 255.0f) & 0xFF) * 0.002f;
            pendingShards_.push_back({e.model, p, delay});
        }
    }
    for (size_t i = 0; i < pendingShards_.size();) {
        PendingShard& ps = pendingShards_[i];
        ps.delay -= deltaTime;
        if (ps.delay > 0.0f) { ++i; continue; }
        // One model, played once and gone (0x7FB2D0, 0x7FC200).
        KitSpawn s;
        s.feetPos = ps.pos;
        s.atPoint = true;
        spawnKitModel(0, ps.model, SLOT_BASE, /*world=*/true, s);
        pendingShards_[i] = std::move(pendingShards_.back());
        pendingShards_.pop_back();
    }
}

// ---------------------------------------------------------------------------
// Camera shakes (SpellEffectCameraShakes -> CameraShakes, 0x7FA620)
// ---------------------------------------------------------------------------
void SpellVisualSystem::loadCameraShakes() {
    if (cameraShakesLoaded_ || !cachedAssetManager_) return;
    cameraShakesLoaded_ = true;
    auto shakes = cachedAssetManager_->loadDBC("CameraShakes.dbc");
    auto spellShakes = cachedAssetManager_->loadDBC("SpellEffectCameraShakes.dbc");
    if (!shakes || !shakes->isLoaded() || shakes->getFieldCount() < 8 || !spellShakes ||
        !spellShakes->isLoaded() || spellShakes->getFieldCount() < 4)
        return;
    std::unordered_map<uint32_t, CameraShakeRow> rows;
    for (uint32_t i = 0; i < shakes->getRecordCount(); ++i) {
        CameraShakeRow r;
        r.type = shakes->getInt32(i, 1);
        r.direction = shakes->getInt32(i, 2);
        r.amplitude = shakes->getFloat(i, 3);
        r.frequency = shakes->getFloat(i, 4);
        r.duration = shakes->getFloat(i, 5);
        r.phase = shakes->getFloat(i, 6);
        r.coefficient = shakes->getFloat(i, 7);
        rows[shakes->getUInt32(i, 0)] = r;
    }
    for (uint32_t i = 0; i < spellShakes->getRecordCount(); ++i) {
        auto& list = spellShakes_[spellShakes->getUInt32(i, 0)];
        for (uint32_t k = 1; k <= 3; ++k)
            if (auto it = rows.find(spellShakes->getUInt32(i, k)); it != rows.end())
                list.push_back(it->second);
    }
}

void SpellVisualSystem::playCameraShake(uint32_t spellShakeId, const glm::vec3& source) {
    if (spellShakeId == 0 || !renderer_) return;
    loadCameraShakes();
    auto it = spellShakes_.find(spellShakeId);
    if (it == spellShakes_.end()) return;
    auto* cc = renderer_->getCameraController();
    if (!cc) return;
    for (const auto& r : it->second)
        cc->addStockShake(r.type, r.direction, r.amplitude, r.frequency, r.duration, r.phase,
                          r.coefficient, source);
}

} // namespace rendering
} // namespace wowee
