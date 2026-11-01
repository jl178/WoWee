#include "rendering/quest_marker_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <cmath>
#include <unordered_set>

namespace wowee {
namespace rendering {

namespace {

/// The stock model for each Kind, and what to draw when a client's data does
/// not have it. The blue and grey variants are 2.x/3.x art; the gold ! and ?
/// go back to 1.x.
struct ModelSource {
    const char* path;
    const char* fallback;
};
constexpr ModelSource kModels[] = {
    {"Interface\\Buttons\\TalkToMe.m2",                nullptr},
    {"Interface\\Buttons\\TalkToMeGrey.m2",            "Interface\\Buttons\\TalkToMe.m2"},
    {"Interface\\Buttons\\TalkToMeBlue.m2",            "Interface\\Buttons\\TalkToMe.m2"},
    {"Interface\\Buttons\\TalkToMeQuestionMark.m2",    nullptr},
    {"Interface\\Buttons\\TalkToMeQuestion_LTBlue.m2", "Interface\\Buttons\\TalkToMeQuestionMark.m2"},
    {"Interface\\Buttons\\TalkToMeQuestion_Grey.m2",   "Interface\\Buttons\\TalkToMeQuestionMark.m2"},
};
static_assert(sizeof(kModels) / sizeof(kModels[0]) ==
              static_cast<size_t>(QuestMarkerRenderer::Kind::Count));

/// Gap between the top of the unit and the model's origin. The mark's own
/// geometry starts 0.155 above its origin and its Stand bob dips 0.09, so
/// this leaves roughly half a yard of air over the head, clear of the name.
constexpr float kHeadClearance = 0.35f;

pipeline::M2Model loadMarkerModel(pipeline::AssetManager* assets, const char* path) {
    // Optional: a 1.x client has no grey or blue marks, and the fallback below
    // says so once rather than the asset manager reporting a missing file.
    const auto data = assets->readFileOptional(path);
    if (data.empty()) return {};
    pipeline::M2Model model = pipeline::M2Loader::load(data);
    if (model.vertices.empty()) return {};
    if (model.name.empty()) model.name = path;
    if (model.version >= 264) {
        const auto skin = assets->readFileOptional(pipeline::skinPathForM2(path));
        if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
    }
    return model;
}

}  // namespace

bool QuestMarkerRenderer::ensureModel(M2Renderer* m2, pipeline::AssetManager* assets, Kind kind) {
    const size_t k = static_cast<size_t>(kind);
    const uint32_t modelId = MODEL_ID_BASE + static_cast<uint32_t>(k);
    if (m2->hasModel(modelId)) return true;
    if (failed_[k] || !assets) return false;

    const ModelSource& src = kModels[k];
    const char* used = src.path;
    pipeline::M2Model model = loadMarkerModel(assets, src.path);
    if (model.vertices.empty() && src.fallback) {
        used = src.fallback;
        model = loadMarkerModel(assets, src.fallback);
        if (!model.vertices.empty()) {
            LOG_WARNING("Quest marker: ", src.path, " is not in this client's data - drawing ",
                        src.fallback, " instead");
        }
    }
    if (model.vertices.empty() || !m2->loadModel(model, modelId)) {
        failed_[k] = true;
        LOG_WARNING("Quest marker: could not load ", src.path, " - that mark will not be shown");
        return false;
    }
    // A map change or the unused-model reaper would otherwise take it between
    // one quest giver going out of sight and the next coming into it.
    m2->setModelPinned(modelId, true);
    LOG_INFO("Quest marker: loaded ", used);
    return true;
}

void QuestMarkerRenderer::update(M2Renderer* m2, pipeline::AssetManager* assets,
                                 const glm::vec3& cameraPos, const std::vector<Marker>& markers) {
    if (m2 != m2_) {
        // A different renderer holds none of the instances kept here.
        instances_.clear();
        m2_ = m2;
    }
    if (!m2) return;
    if (markers.empty() && instances_.empty()) return;

    std::unordered_set<uint64_t> wanted;
    wanted.reserve(markers.size());
    for (const auto& mark : markers) {
        const size_t k = static_cast<size_t>(mark.kind);
        if (k >= KIND_COUNT) continue;
        if (!ensureModel(m2, assets, mark.kind)) continue;
        wanted.insert(mark.guid);

        const glm::vec3 anchor(mark.position.x, mark.position.y,
                               mark.position.z + mark.headTop + kHeadClearance);

        auto it = instances_.find(mark.guid);
        if (it != instances_.end() &&
            (it->second.kind != mark.kind || !m2->hasInstance(it->second.instanceId))) {
            // A different mark now (the quest was accepted, or completed), or
            // the instance went with a renderer-wide clear.
            m2->removeInstance(it->second.instanceId);
            instances_.erase(it);
            it = instances_.end();
        }
        if (it == instances_.end()) {
            const uint32_t id = m2->createInstance(MODEL_ID_BASE + static_cast<uint32_t>(k),
                                                   anchor, glm::vec3(0.0f), 1.0f,
                                                   /*allowPositionDedup=*/false);
            if (id == 0) continue;
            // Something to look at, not to walk on or stand under: no
            // collision, no shadow on the ground, and never culled before the
            // unit it belongs to.
            m2->setSkipCollision(id, true);
            m2->setInstanceIsGameObject(id, true);
            m2->setInstanceCastsShadow(id, false);
            Placed placed;
            placed.instanceId = id;
            placed.kind = mark.kind;
            it = instances_.emplace(mark.guid, placed).first;
        }

        // The model's bone is a Z-locked cylindrical billboard (flags 0x240),
        // which the M2 renderer does not evaluate: turn the instance about Z so
        // the mark's face (+X in model space) looks at the camera. Building
        // the basis from the direction to the camera keeps the ? reading the
        // right way round from every side. Straight overhead the direction is
        // undefined, so the last one is kept.
        const glm::vec2 toCamera(cameraPos.x - anchor.x, cameraPos.y - anchor.y);
        const float len = glm::length(toCamera);
        if (len > 1e-3f) it->second.facing = toCamera / len;
        const glm::vec2 f = it->second.facing;
        glm::mat4 transform(1.0f);
        transform[0] = glm::vec4(f.x, f.y, 0.0f, 0.0f);   // +X toward the camera
        transform[1] = glm::vec4(-f.y, f.x, 0.0f, 0.0f);  // Z x X
        transform[2] = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
        transform[3] = glm::vec4(anchor, 1.0f);
        m2->setInstanceTransform(it->second.instanceId, transform);
    }

    for (auto it = instances_.begin(); it != instances_.end();) {
        if (wanted.count(it->first) == 0) {
            m2->removeInstance(it->second.instanceId);
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
}

void QuestMarkerRenderer::clear() {
    if (m2_) {
        for (const auto& [guid, placed] : instances_) m2_->removeInstance(placed.instanceId);
    }
    instances_.clear();
}

}  // namespace rendering
}  // namespace wowee
