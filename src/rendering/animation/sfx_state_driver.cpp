// ============================================================================
// SfxStateDriver - extracted from AnimationController
//
// Tracks state transitions for activity SFX (jump, landing, swim) and
// mount ambient sounds.  Moved from AnimationController::updateSfxState().
// ============================================================================

#include "rendering/animation/sfx_state_driver.hpp"
#include "rendering/animation/footstep_driver.hpp"
#include "rendering/renderer.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "rendering/camera_controller.hpp"

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

void SfxStateDriver::update(float deltaTime, Renderer* renderer,
                            bool mounted, bool taxiFlight,
                            FootstepDriver& footstepDriver) {
    auto* activitySoundManager = renderer->getAudioCoordinator()->getActivitySoundManager();
    if (!activitySoundManager) return;

    auto* cameraController = renderer->getCameraController();

    activitySoundManager->update(deltaTime);
    if (cameraController && cameraController->isThirdPerson()) {
        bool grounded = cameraController->isGrounded();
        bool falling = cameraController->isFalling();
        bool swimming = cameraController->isSwimming();
        bool moving = cameraController->isMoving();

        if (!initialized_) {
            prevGrounded_ = grounded;
            prevFalling_ = falling;
            prevSwimming_ = swimming;
            initialized_ = true;
        }

        if (!swimming && prevSwimming_) sinceWaterExit_ = 0.0f;
        else if (sinceWaterExit_ < kWaterExitSfxSuppress) sinceWaterExit_ += deltaTime;
        const bool justLeftWater = sinceWaterExit_ < kWaterExitSfxSuppress;

        // Jump detection. Wading ashore lifts the character without a jump, and
        // jumpClips are the character's exertion vocalisations, so a grunt there
        // sounds like it jumped out of the lake.
        // No sound on takeoff: the stock client plays nothing when a player
        // jumps. The MainJump clips are in the data but not used for this.

        // Landing detection. Wading out of water reaches the ground without a
        // fall, and retail marks that with the water-exit splash below rather
        // than a landing thud, so a swim-to-ground transition is not a landing.
        const glm::vec3 pos = renderer->getCharacterPosition();
        const float z = pos.z;
        // A teleport is not a fall: the drop is measured from where the fall
        // began (0x986fb0), and a jump of tens of yards in one frame starts
        // it again.
        const glm::vec2 step(pos.x - lastPos_.x, pos.y - lastPos_.y);
        const bool teleported = glm::dot(step, step) > 20.0f * 20.0f || std::abs(pos.z - lastPos_.z) > 20.0f;
        lastPos_ = pos;
        if (teleported) airTopZ_ = z;
        if (!grounded) {
            airTopZ_ = (prevGrounded_ || teleported) ? z : std::max(airTopZ_, z);
        }
        if (grounded && !prevGrounded_ && !justLeftWater) {
            // A hard landing is a drop of more than 13 yards (0x73d3d0
            // against 0xa349f4; 70 for the dead-or-flagged case).
            bool hardLanding = prevFalling_ && (airTopZ_ - z) > 13.0f;
            activitySoundManager->playLanding(
                footstepDriver.resolveFootstepSurface(renderer), hardLanding);
        }

        // Water transitions
        if (swimming && !prevSwimming_) {
            activitySoundManager->playWaterEnter();
        } else if (!swimming && prevSwimming_) {
            activitySoundManager->playWaterExit();
        }

        activitySoundManager->setSwimmingState(swimming, moving);

        // The music is not turned down in the water by the stock client (no
        // ducking row on the underwater loop, no music slot for it); the old
        // 30% swim duck stays for data without the tables.
        if (renderer->getAudioCoordinator()->getMusicManager()) {
            const bool tables = renderer->getAudioCoordinator()->getSoundData() != nullptr;
            renderer->getAudioCoordinator()->getMusicManager()->setUnderwaterMode(swimming && !tables);
        }

        fallSeconds_ = falling ? fallSeconds_ + deltaTime : 0.0f;
        prevGrounded_ = grounded;
        prevFalling_ = falling;
        prevSwimming_ = swimming;
    } else {
        activitySoundManager->setSwimmingState(false, false);
        if (renderer->getAudioCoordinator()->getMusicManager()) {
            renderer->getAudioCoordinator()->getMusicManager()->setUnderwaterMode(false);
        }
        initialized_ = false;
    }

    // Mount ambient sounds
    if (renderer->getAudioCoordinator()->getMountSoundManager()) {
        renderer->getAudioCoordinator()->getMountSoundManager()->update(deltaTime);
        if (cameraController && mounted) {
            bool isMoving = cameraController->isMoving();
            bool flying = taxiFlight || !cameraController->isGrounded();
            renderer->getAudioCoordinator()->getMountSoundManager()->setMoving(isMoving);
            renderer->getAudioCoordinator()->getMountSoundManager()->setFlying(flying);
        }
    }
}

} // namespace rendering
} // namespace wowee
