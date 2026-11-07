// How game objects present themselves, the stock 3.3.5a way: the animation
// kind their state asks for (rendering/animation/game_object_anim.hpp), the
// sounds their models' events make (audio/game_object_sounds.hpp), the
// freeze flag and the quest sparkle. docs/re/interactions.md, section 12.

#include "core/entity_spawner.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/game_object_sounds.hpp"
#include "game/game_handler.hpp"
#include "game/update_field_table.hpp"
#include "rendering/animation/game_object_anim.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"

#include <cstring>

namespace wowee {
namespace core {

namespace goanim = rendering::anim::goanim;

void EntitySpawner::ensureGameObjectSounds() {
    if (!goSounds_) {
        goSounds_ = std::make_unique<audio::GameObjectSounds>();
        goSounds_->loadDisplaySounds(assetManager_);
    }
    const audio::SoundData* data = (gameServices_ && gameServices_->audioCoordinator)
                                       ? gameServices_->audioCoordinator->getSoundData()
                                       : nullptr;
    goSounds_->setSoundData(data);
}

void EntitySpawner::playGameObjectKind(GameObjectInstanceInfo& info, uint8_t kindByte, bool heard) {
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!m2 || info.isWmo) return;
    const auto kind = static_cast<goanim::Kind>(kindByte);
    const uint32_t inst = info.instanceId;
    // The same kind again plays nothing new (0x70d510).
    if (info.goKind == kindByte) return;
    const auto prevKind = static_cast<goanim::Kind>(info.goKind);
    const bool hadKind = info.goKind != 0xFF;

    auto has = [&](uint32_t id) { return m2->hasAnimation(inst, id); };
    const uint32_t first = m2->firstSequenceId(inst);
    uint32_t requested = 0;
    const auto play = goanim::resolveKind(has, kind, first, &requested);

    // What follows it: a motion runs into its state's loop, a custom
    // animation or Spawn goes back to the state's own kind.
    std::optional<goanim::Kind> then;
    if (goanim::isMotion(kind)) then = goanim::kindAfterEnd(kind);
    else if (!goanim::isStateLoop(kind) && kind != goanim::Kind::Despawn)
        then = goanim::kindForState(info.goState == 0xFF ? goanim::kStateReady : info.goState);
    uint32_t thenId = UINT32_MAX;
    uint8_t thenMode = 0;
    if (then) {
        const auto t = goanim::resolveKind(has, *then, first);
        thenId = t.id;
        thenMode = static_cast<uint8_t>(t.mode);
    }

    // A motion that undoes the one under way starts where that one has got
    // to (0x70d510 -> 0x70d010): a door shut half way through opening.
    float start = 0.0f;
    if (hadKind && goanim::isReversal(prevKind, kind)) {
        if (const auto progress = m2->instanceOneShotProgress(inst)) start = 1.0f - *progress;
    }

    // A model without the kind's own sequence: when the id it asks for
    // instead is already playing, it is left alone (0x70d1e0 compares the
    // id before SetBoneSequence's AnimationData walk with the current one).
    if (!has(goanim::kindAnimation(kind)) && m2->currentSequenceId(inst) == requested) {
        info.goKind = kindByte;
        return;
    }

    // Blending as SetBoneSequence's primary flag: everything but Spawn.
    const bool blend = hadKind && kind != goanim::Kind::Spawn;
    m2->playInstanceSequence(inst, play.id, static_cast<uint8_t>(play.mode), thenId, thenMode,
                             start, blend, heard);
    // The kind the next change is compared with. A motion keeps its name
    // after it has run into its loop; a reversal then finds no one-shot
    // under way and starts from the beginning, as 0x70d510 does once the
    // motion is over.
    info.goKind = kindByte;
    if (info.frozenByFlag && goanim::isMotion(kind)) m2->setInstanceAnimationPaused(inst, true);
}

void EntitySpawner::playGameObjectCustomAnim(uint64_t guid, uint32_t animationId) {
    auto it = gameObjectInstances_.find(guid);
    if (it == gameObjectInstances_.end() || it->second.isWmo) return;
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!m2) return;
    // Only a model that has the sequence plays it (0x70c480); nothing falls
    // back for a custom animation.
    if (!m2->hasAnimation(it->second.instanceId, animationId)) return;
    uint8_t kind = 0xFF;
    if (animationId >= rendering::anim::CUSTOM_0 && animationId <= rendering::anim::CUSTOM_3)
        kind = static_cast<uint8_t>(goanim::Kind::Custom0) +
               static_cast<uint8_t>(animationId - rendering::anim::CUSTOM_0);
    else if (animationId == rendering::anim::DESPAWN)
        kind = static_cast<uint8_t>(goanim::Kind::Despawn);
    else
        return;
    // A custom animation replays when asked again: it is a performance, not
    // a state (0x70c480 goes through 0x70d510, which compares kinds, but a
    // custom kind is cleared when it ends).
    it->second.goKind = 0xFE;
    playGameObjectKind(it->second, kind, /*heard=*/true);
}

void EntitySpawner::updateGameObjectPresentation() {
    auto* m2 = renderer_ ? renderer_->getM2Renderer() : nullptr;
    if (!m2) return;
    ensureGameObjectSounds();

    // The events the models passed since the last frame.
    for (const auto& ev : m2->takeObjectEvents()) {
        goSounds_->onEvent(ev.instanceId, ev.tag, ev.data, ev.position);
    }
    goSounds_->update([m2](uint32_t id, glm::mat4& out) { return m2->getInstanceModelMatrix(id, out); });

    // GO_FLAG_FREEZE_ANIMATION (0x80): a motion stops where it is while the
    // flag is up (0x70d160).
    if (!gameHandler_) return;
    const uint16_t flagsIdx = game::fieldIndex(game::UF::GAMEOBJECT_FLAGS);
    const uint16_t dynIdx = game::fieldIndex(game::UF::GAMEOBJECT_DYNAMIC);
    if (flagsIdx == 0xFFFF) return;
    auto& em = gameHandler_->getEntityManager();
    for (auto& [guid, info] : gameObjectInstances_) {
        auto entity = em.getEntity(guid);
        if (!entity) continue;
        // The per-player flags (activate, sparkle) as they come and go: what a
        // quest object's glitter answers to.
        if (dynIdx != 0xFFFF && entity->hasField(dynIdx)) {
            const uint32_t dyn = entity->getField(dynIdx) & 0xFFFFu;
            if (dyn != info.dynamicFlags) {
                if (info.dynamicFlags != 0xFFFFFFFFu || dyn != 0) {
                    static core::LogBudget dynBudget(60, "game object dynamic flag changes");
                    if (dynBudget.take())
                        LOG_INFO("GO dynamic flags: guid=0x", std::hex, guid, " 0x", dyn, std::dec,
                                 (dyn & 0x8u) ? " (sparkle)" : "");
                }
                info.dynamicFlags = dyn;
            }
        }
        if (info.isWmo || !entity->hasField(flagsIdx)) continue;
        const bool freeze = (entity->getField(flagsIdx) & goanim::kFlagFreezeAnimation) != 0;
        if (freeze == info.frozenByFlag) continue;
        info.frozenByFlag = freeze;
        m2->setInstanceAnimationPaused(info.instanceId, freeze);
    }
}

std::vector<EntitySpawner::SparklingObject> EntitySpawner::sparklingGameObjects() const {
    std::vector<SparklingObject> out;
    if (!gameHandler_) return out;
    const uint16_t dynIdx = game::fieldIndex(game::UF::GAMEOBJECT_DYNAMIC);
    if (dynIdx == 0xFFFF) return out;
    const uint16_t scaleIdx = game::fieldIndex(game::UF::OBJECT_FIELD_SCALE_X);
    auto& em = gameHandler_->getEntityManager();
    for (const auto& [guid, info] : gameObjectInstances_) {
        auto entity = em.getEntity(guid);
        if (!entity || !entity->hasField(dynIdx)) continue;
        // GO_DYNFLAG_LO_SPARKLE (0x8): the server sets it for a player whose
        // quest needs the object (0x712b40 -> 0x711210).
        if ((entity->getField(dynIdx) & 0x8u) == 0) continue;
        float scale = 1.0f;
        if (scaleIdx != 0xFFFF && entity->hasField(scaleIdx)) {
            const uint32_t raw = entity->getField(scaleIdx);
            float f = 0.0f;
            std::memcpy(&f, &raw, sizeof(f));
            if (f > 0.0f && f < 100.0f) scale = f;
        }
        SparklingObject s;
        s.guid = guid;
        s.position = core::coords::canonicalToRender(
            glm::vec3(entity->getX(), entity->getY(), entity->getZ()));
        // HARDCODED Loot Art at the object's scale x 1.15 (0x70d080, 0xa33564).
        s.scale = scale * 1.15f;
        out.push_back(s);
    }
    return out;
}

} // namespace core
} // namespace wowee
