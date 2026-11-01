#include "rendering/spell_visual_system.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/placement_transform.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "pipeline/dbc_layout.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>

namespace wowee {
namespace rendering {

void SpellVisualSystem::initialize(M2Renderer* m2Renderer, Renderer* renderer) {
    m2Renderer_ = m2Renderer;
    renderer_ = renderer;
}

void SpellVisualSystem::shutdown() {
    reset();
    m2Renderer_ = nullptr;
    renderer_ = nullptr;
    cachedAssetManager_ = nullptr;
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
            kitEffects += static_cast<uint32_t>(row.effects.size());
            if (!row.effects.empty() || row.animId) kits_[kitId] = std::move(row);
        }
    }

    // Load SpellVisual.dbc - visualId → its kits and missile
    auto svDbc = cachedAssetManager_->loadDBC("SpellVisual.dbc");
    if (!svDbc || !svDbc->isLoaded()) {
        LOG_DEBUG("SpellVisual: SpellVisual.dbc unavailable");
        return;
    }
    uint32_t svFc = svDbc->getFieldCount();
    if (svFc == 32) {
        if (svMissileAttachField == kNone) svMissileAttachField = 16;
        if (svMissileDestField == kNone)   svMissileDestField = 10;
    }
    auto readField = [&](uint32_t row, uint32_t field) -> uint32_t {
        return field < svFc ? svDbc->getUInt32(row, field) : 0u;
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
        if (svMissileAttachField < svFc)
            v.missileAttachment = svDbc->getInt32(i, svMissileAttachField);
        if (svMissileDestField < svFc)
            v.missileDestination = svDbc->getInt32(i, svMissileDestField);
        // The missile flies from caster to target; it is never a stand-in
        // for a missing cast or impact kit.
        if (const uint32_t eff = readField(i, svMissileField)) {
            auto fxIt = effectPaths.find(eff);
            if (fxIt != effectPaths.end()) { v.missilePath = fxIt->second; ++loadedMissile; }
        }
        visuals_[vid] = std::move(v);
    }
    LOG_INFO("SpellVisual: loaded ", visuals_.size(), " visuals, ", kits_.size(), " kits (",
             kitEffects, " effects), ", loadedMissile, " missiles");
}

// ---------------------------------------------------------------------------
// Kit slot → model attachment point
// ---------------------------------------------------------------------------
// Every creature and character model carries the spell attachment set
// 15 ChestBloodFront, 17 Breath, 19 Base, 20 Head, 21 SpellLeftHand,
// 22 SpellRightHand (checked on humanmale, orcmale, kobold, wolf, murloc,
// dragonwhelp); the older ids follow as fallbacks. 11 (Helm) is missing on
// creatures, so a head effect aimed there fell back to the feet.
namespace {
struct SlotAttach { uint32_t ids[3]; float fallbackHeight; };
constexpr SlotAttach kSlotAttach[SpellVisualSystem::SLOT_COUNT] = {
    /* HEAD         */ {{20, 11, 0},  1.9f},
    /* CHEST        */ {{15, 34, 0},  1.3f},
    /* BASE         */ {{19, 0, 0},   0.0f},
    /* LEFT_HAND    */ {{21, 2, 0},   1.0f},
    /* RIGHT_HAND   */ {{22, 1, 0},   1.0f},
    /* BREATH       */ {{17, 20, 11}, 1.7f},
    /* LEFT_WEAPON  */ {{2, 21, 0},   1.0f},
    /* RIGHT_WEAPON */ {{1, 22, 0},   1.0f},
    /* SPECIAL0..2  */ {{19, 0, 0},   0.0f}, {{19, 0, 0}, 0.0f}, {{19, 0, 0}, 0.0f},
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

// One kit on one unit: each slot the kit fills gets its own model at that
// slot's attachment, riding it from then on.
void SpellVisualSystem::spawnKit(uint32_t kitId, const glm::vec3& feetPos,
                                 uint32_t attachInstanceId, bool isPrecast,
                                 uint64_t ownerGuid, float lifetime) {
    if (!m2Renderer_ || kitId == 0) return;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!cachedAssetManager_) return;
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto kitIt = kits_.find(kitId);
    if (kitIt == kits_.end()) return;

    for (const auto& fx : kitIt->second.effects) {
        const uint32_t modelId = loadSpellModel(fx.path);
        if (modelId == 0) continue;
        glm::mat4 xf;
        const bool attached = slotTransform(fx.slot, attachInstanceId, feetPos, xf);
        const uint32_t instanceId =
            m2Renderer_->createInstance(modelId, glm::vec3(xf[3]), glm::vec3(0.0f), 1.0f);
        if (instanceId == 0) {
            LOG_WARNING("SpellVisual: createInstance failed for kit ", kitId, " model ", fx.path);
            continue;
        }
        if (attached) m2Renderer_->setInstanceTransform(instanceId, xf);
        m2Renderer_->restartInstanceAnimation(instanceId);
        float duration = lifetime;
        if (duration <= 0.0f) {
            const float animDurMs = m2Renderer_->getInstanceAnimDuration(instanceId);
            duration = (animDurMs > 100.0f)
                ? std::clamp(animDurMs / 1000.0f, 0.5f, SPELL_VISUAL_MAX_DURATION)
                : SPELL_VISUAL_DEFAULT_DURATION;
        }
        activeSpellVisuals_.push_back({.instanceId = instanceId, .elapsed = 0.0f,
                                       .duration = duration, .isPrecast = isPrecast,
                                       .slot = fx.slot,
                                       .attachInstanceId = attached ? attachInstanceId : 0u,
                                       .ownerGuid = ownerGuid});
        LOG_DEBUG("SpellVisual: kit ", kitId, " slot ", static_cast<int>(fx.slot), " model ",
                  fx.path, " attached=", attached, " duration=", duration, "s");
    }
}

void SpellVisualSystem::playSpellVisualKit(uint32_t kitId, const glm::vec3& worldPosition,
                                           uint32_t attachInstanceId) {
    spawnKit(kitId, worldPosition, attachInstanceId, false, 0, 0.0f);
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
    out.precast = anim(vIt->second.precastKit);
    out.cast    = anim(vIt->second.castKit);
    out.channel = anim(vIt->second.channelKit);
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
    const float lifetime = (durationMs > 0)
        ? std::min(static_cast<float>(durationMs) / 1000.0f + 1.0f, SPELL_PRECAST_MAX_DURATION)
        : 1.0f;
    spawnKit(kitId, worldPosition, attachInstanceId, /*isPrecast=*/true, casterGuid, lifetime);
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
        LOG_INFO("SpellVisual: loaded model id=", modelId, " path=", modelPath);
    }
    return modelId;
}

void SpellVisualSystem::playSpellVisual(uint32_t visualId, const glm::vec3& worldPosition,
                                         bool useImpactKit, uint32_t attachInstanceId) {
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
    const uint32_t kitId = useImpactKit ? vIt->second.impactKit : vIt->second.castKit;
    spawnKit(kitId, worldPosition, attachInstanceId, false, 0, 0.0f);
}

bool SpellVisualSystem::hasSpellMissile(uint32_t visualId) {
    if (visualId == 0) return false;
    if (!cachedAssetManager_)
        cachedAssetManager_ = core::Application::getInstance().getAssetManager();
    if (!spellVisualDbcLoaded_) loadSpellVisualDbc();
    auto it = visuals_.find(visualId);
    return it != visuals_.end() && !it->second.missilePath.empty();
}

bool SpellVisualSystem::playSpellMissile(uint32_t visualId, const glm::vec3& start,
                                         const glm::vec3& end, float speed, bool impactAtEnd,
                                         uint32_t casterInstanceId, uint32_t targetInstanceId) {
    if (!m2Renderer_ || !hasSpellMissile(visualId)) return false;
    if (!cachedAssetManager_) return false;
    if (!std::isfinite(speed) || speed <= 0.0f) return false;

    const VisualRow& v = visuals_.at(visualId);
    const uint32_t modelId = loadSpellModel(v.missilePath);
    if (modelId == 0) return false;

    // Launch point: the caster attachment SpellVisual names, or for the
    // default (-1, nearly every bolt) the point between the casting hands -
    // in front of the caster as SpellCastDirected brings them together.
    glm::vec3 launch;
    glm::mat4 xf;
    if (v.missileAttachment >= 0 && v.missileAttachment < SLOT_COUNT) {
        slotTransform(static_cast<uint8_t>(v.missileAttachment), casterInstanceId, start, xf);
        launch = glm::vec3(xf[3]);
    } else {
        glm::mat4 l, r;
        const bool hasL = slotTransform(SLOT_LEFT_HAND, casterInstanceId, start, l);
        const bool hasR = slotTransform(SLOT_RIGHT_HAND, casterInstanceId, start, r);
        if (hasL && hasR)  launch = (glm::vec3(l[3]) + glm::vec3(r[3])) * 0.5f;
        else if (hasR)     launch = glm::vec3(r[3]);
        else if (hasL)     launch = glm::vec3(l[3]);
        else { slotTransform(SLOT_CHEST, 0, start, xf); launch = glm::vec3(xf[3]); }
    }
    const int32_t destSlot = (v.missileDestination >= 0 && v.missileDestination < SLOT_COUNT)
        ? v.missileDestination : static_cast<int32_t>(SLOT_CHEST);
    slotTransform(static_cast<uint8_t>(destSlot), targetInstanceId, end, xf);
    const glm::vec3 aim = glm::vec3(xf[3]);

    const uint32_t instanceId = m2Renderer_->createInstance(modelId, launch, glm::vec3(0.0f), 1.0f);
    if (instanceId == 0) {
        LOG_WARNING("SpellVisual: failed to create missile instance for visualId=", visualId);
        return false;
    }
    m2Renderer_->restartInstanceAnimation(instanceId);

    spellMissiles_.push_back({.instanceId = instanceId, .visualId = visualId, .pos = launch,
                              .end = aim, .endFeet = end, .targetInstanceId = targetInstanceId,
                              .destSlot = destSlot, .speed = speed, .elapsed = 0.0f,
                              .impactAtEnd = impactAtEnd});
    const glm::vec3 d = aim - launch;
    const float distance = glm::length(d);
    placeSpellMissile(spellMissiles_.back(), distance > 1e-4f ? d / distance : glm::vec3(1, 0, 0));
    LOG_DEBUG("SpellVisual: missile visualId=", visualId, " model=", v.missilePath,
              " distance=", distance, " speed=", speed, " eta=", distance / speed, "s");
    return true;
}

// The missile models point down +X. Turn it to face along its path, with the
// nose pitched up or down to follow a target above or below the caster.
void SpellVisualSystem::placeSpellMissile(const SpellMissile& missile, const glm::vec3& dir) {
    glm::vec3 rotation(0.0f);
    rotation.z = std::atan2(dir.y, dir.x);
    rotation.y = -std::asin(std::clamp(dir.z, -1.0f, 1.0f));
    m2Renderer_->setInstanceTransform(missile.instanceId,
                                      placementModelMatrix(missile.pos, rotation, 1.0f));
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

void SpellVisualSystem::update(float deltaTime) {
    if (activeSpellVisuals_.empty() && physicalProjectiles_.empty() && spellMissiles_.empty())
        return;

    // Missiles home on the target's attachment at the spell's speed, so one
    // fired at a running target still lands on it. Arrivals are collected
    // first: the impact spawns into activeSpellVisuals_ on the target.
    struct Arrival { uint32_t visualId; glm::vec3 feet; uint32_t targetInstanceId; };
    std::vector<Arrival> arrivals;
    for (auto it = spellMissiles_.begin(); it != spellMissiles_.end(); ) {
        it->elapsed += deltaTime;
        if (it->targetInstanceId != 0) {
            glm::mat4 xf;
            if (slotTransform(static_cast<uint8_t>(it->destSlot), it->targetInstanceId,
                              it->endFeet, xf))
                it->end = glm::vec3(xf[3]);
        }
        const glm::vec3 toEnd = it->end - it->pos;
        const float remaining = glm::length(toEnd);
        const float step = it->speed * deltaTime;
        // A missile still flying after this long has lost its target to a
        // teleport or a despawn; the server settled the hit long ago.
        if (remaining <= step || remaining < 1e-3f || it->elapsed >= SPELL_MISSILE_MAX_DURATION) {
            m2Renderer_->removeInstance(it->instanceId);
            if (it->impactAtEnd)
                arrivals.push_back({it->visualId, it->endFeet, it->targetInstanceId});
            it = spellMissiles_.erase(it);
        } else {
            const glm::vec3 dir = toEnd / remaining;
            it->pos += dir * step;
            placeSpellMissile(*it, dir);
            ++it;
        }
    }
    for (const auto& a : arrivals)
        playSpellVisual(a.visualId, a.feet, /*useImpactKit=*/true, a.targetInstanceId);

    // Get character bone tracking context (once per frame)
    CharacterRenderer* charRenderer = renderer_ ? renderer_->getCharacterRenderer() : nullptr;

    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        it->elapsed += deltaTime;
        if (it->elapsed >= it->duration) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            // Ride the unit's attachment point - position, facing and scale -
            // the way the stock client parents kit models to the unit.
            if (it->attachInstanceId != 0 && charRenderer) {
                glm::mat4 xf;
                if (slotTransform(it->slot, it->attachInstanceId, glm::vec3(0.0f), xf))
                    m2Renderer_->setInstanceTransform(it->instanceId, xf);
            }
            ++it;
        }
    }


    if (charRenderer) {
        for (auto it = physicalProjectiles_.begin(); it != physicalProjectiles_.end(); ) {
            it->elapsed += deltaTime;
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
                it = physicalProjectiles_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void SpellVisualSystem::cancelPrecastVisuals(uint64_t casterGuid) {
    if (!m2Renderer_ || casterGuid == 0) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast && it->ownerGuid == casterGuid) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::cancelAllPrecastVisuals() {
    if (!m2Renderer_) return;
    for (auto it = activeSpellVisuals_.begin(); it != activeSpellVisuals_.end(); ) {
        if (it->isPrecast) {
            m2Renderer_->removeInstance(it->instanceId);
            it = activeSpellVisuals_.erase(it);
        } else {
            ++it;
        }
    }
}

void SpellVisualSystem::reset() {
    // Clear lingering spell visual instances from the previous map/combat session.
    // Without this, old effects could remain visible after teleport or map change.
    for (auto& sv : activeSpellVisuals_) {
        if (m2Renderer_) m2Renderer_->removeInstance(sv.instanceId);
    }
    activeSpellVisuals_.clear();
    for (const auto& missile : spellMissiles_) {
        if (m2Renderer_) m2Renderer_->removeInstance(missile.instanceId);
    }
    spellMissiles_.clear();
    if (renderer_ && renderer_->getCharacterRenderer()) {
        for (const auto& projectile : physicalProjectiles_)
            renderer_->getCharacterRenderer()->removeInstance(projectile.instanceId);
    }
    physicalProjectiles_.clear();
    // Reset the negative cache so models that failed during asset loading can retry.
    spellVisualFailedModels_.clear();
}

} // namespace rendering
} // namespace wowee
