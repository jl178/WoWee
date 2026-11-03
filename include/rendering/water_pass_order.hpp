#pragma once

/// Where the water draws: in the scene pass, or in a continuation after it.
///
/// The continuation exists so that the water's refraction copy, taken between
/// the two, holds the scene without the water. Soft particles (E4) add a
/// second reason: their late pass draws the blended particles once the scene
/// pass has closed, and anything still in the scene pass ends up under them -
/// a fire's smoke under a lake's surface showed through it at full strength.
/// Stock draws particles, then water, then weather. So with soft particles on
/// the water always leaves the scene pass when it can, and the weather goes
/// with it; with them off the rule is the one the client always had.
///
/// Renderer::waterDrawsInContinuePass and syncSwimEffectsTargetPass read it;
/// test_phase2_enhancements holds it.

namespace wowee {
namespace rendering {
namespace water_pass_order {

struct Frame {
    bool msaa = false;            ///< the scene is multisampled
    bool offscreenScene = false;  ///< the scene draws into the post-process target
    bool softParticles = false;   ///< this frame has the late particle pass
    bool water1xPass = false;     ///< the single-sampled water pass exists (MSAA)
    bool water1xKeepDepth = false;  ///< and its depth-keeping sibling
    bool sceneContinuePass = false; ///< the 1x scene continuation pass exists
};

/// Whether the water draws in a pass after the scene pass.
constexpr bool waterAfterScene(const Frame& f) {
    if (f.msaa) {
        // The 1x water pass targets the swapchain; over an off-screen scene
        // only the soft particles' variant (its resolved colour and depth,
        // the depth kept for the post-processing) can take it.
        if (f.offscreenScene) return f.softParticles && f.water1xPass && f.water1xKeepDepth;
        return f.water1xPass;
    }
    return f.sceneContinuePass;
}

/// Whether the weather follows the water out of the scene pass: only for the
/// late particle pass, so its particles stay under the rain as stock has them.
constexpr bool weatherAfterScene(const Frame& f) {
    return f.softParticles && waterAfterScene(f);
}

}  // namespace water_pass_order
}  // namespace rendering
}  // namespace wowee
