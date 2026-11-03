#pragma once

#include "rendering/camera.hpp"
#include "core/input.hpp"
#include "game/stock_movement.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <functional>
#include <optional>

namespace wowee {
namespace rendering {

class TerrainManager;
class WMORenderer;
class M2Renderer;
class WaterRenderer;

class CameraController {
public:
    CameraController(Camera* camera);

    void update(float deltaTime);
    void processMouseMotion(const SDL_MouseMotionEvent& event);
    void processMouseButton(const SDL_MouseButtonEvent& event);
    void releaseMouseCapture();

    void setMovementSpeed(float speed) { movementSpeed = speed; }
    void setMouseSensitivity(float sensitivity) { mouseSensitivity = sensitivity; }
    [[nodiscard]] float getMouseSensitivity() const { return mouseSensitivity; }
    void setInvertMouse(bool invert) { invertMouse = invert; }
    [[nodiscard]] bool isInvertMouse() const { return invertMouse; }
    /// How far back the camera may be pulled, as a multiple of the original
    /// client's own limit.
    ///
    /// Max Camera Distance, as a multiple of the 15 yards the original client
    /// gives (cameraDistanceMax). The schema's cameramaxdistance row counts the yards themselves
    /// and kClientCVars scales cameraDistanceMaxFactor - the CVar the game's
    /// own slider wrote, before that page was retired - onto it. Before both
    /// there was an extended-zoom checkbox of this client's own, a switch
    /// between two constants where this is every distance between them.
    void setMaxDistanceFactor(float factor) {
        maxDistanceFactor_ = std::clamp(factor, 1.0f, kMaxDistanceFactorLimit);
    }
    [[nodiscard]] float maxDistanceFactor() const { return maxDistanceFactor_; }
    [[nodiscard]] bool isExtendedZoom() const { return extendedZoom_; }
    void setEnabled(bool enabled) { this->enabled = enabled; }
    void setTerrainManager(TerrainManager* tm) { terrainManager = tm; }
    void setWMORenderer(WMORenderer* wmo) { wmoRenderer = wmo; }
    void setM2Renderer(M2Renderer* m2) { m2Renderer = m2; }
    void setWaterRenderer(WaterRenderer* wr) { waterRenderer = wr; }

    /// Holds the view still while something else owns the finger that is
    /// moving the mouse. The on-screen stick is that something: SDL reports the
    /// first finger as a held left button wherever it goes, and without this
    /// the camera spins every time the player walks.
    void setRotationSuppressed(bool suppressed) { rotationSuppressed_ = suppressed; }

    /// Turns the view by a drag this controller never saw as a mouse.
    ///
    /// SDL reports only the first finger as a mouse, so with a thumb on the
    /// movement stick a second finger dragging to look produces nothing at all.
    /// The touch controls read that finger themselves and hand the movement
    /// here, in the same pixels and through the same sensitivity and limits a
    /// mouse drag goes through.
    void applyLookDelta(float dxPixels, float dyPixels);

    /// While a finger is steering, the character faces where the view does -
    /// what holding the right mouse button means on a desktop.
    void setSteering(bool steering) { steering_ = steering; }

    void processMouseWheel(float delta);
    void setFollowTarget(glm::vec3* target);
    void setDefaultSpawn(const glm::vec3& position, float yawDeg, float pitchDeg) {
        defaultPosition = position;
        defaultYaw = yawDeg;
        defaultPitch = pitchDeg;
    }

    void reset();
    void resetAngles();
    void teleportTo(const glm::vec3& pos);
    void setOnlineMode(bool online) { onlineMode = online; }

    // Last known safe position (grounded, not falling)
    [[nodiscard]] bool hasLastSafePosition() const { return hasLastSafe_; }
    [[nodiscard]] const glm::vec3& getLastSafePosition() const { return lastSafePos_; }
    [[nodiscard]] float getContinuousFallTime() const { return continuousFallTime_; }

    // Auto-unstuck callback (triggered when falling too long)
    using AutoUnstuckCallback = std::function<void()>;
    void setAutoUnstuckCallback(AutoUnstuckCallback cb) { autoUnstuckCallback_ = std::move(cb); }
    void startIntroPan(float durationSec = 2.8f, float orbitDegrees = 140.0f);
    [[nodiscard]] bool isIntroActive() const { return introActive; }
    [[nodiscard]] bool isIdleOrbit() const { return idleOrbit_; }
    void setIdleOrbitEnabled(bool enabled) {
        idleOrbitEnabled_ = enabled;
        if (!enabled && idleOrbit_) {
            introActive = false;
            idleOrbit_ = false;
            idleTimer_ = 0.0f;
        }
    }
    [[nodiscard]] bool isIdleOrbitEnabled() const { return idleOrbitEnabled_; }

    [[nodiscard]] float getMovementSpeed() const { return movementSpeed; }
    [[nodiscard]] const glm::vec3& getDefaultPosition() const { return defaultPosition; }
    [[nodiscard]] bool isMoving() const;
    [[nodiscard]] float getYaw() const { return yaw; }
    [[nodiscard]] float getPitch() const { return pitch; }
    [[nodiscard]] float getFacingYaw() const { return facingYaw; }
    [[nodiscard]] float getTravelYaw() const { return travelYaw_; }
    [[nodiscard]] bool isThirdPerson() const { return thirdPerson; }
    [[nodiscard]] bool isFirstPersonView() const { return thirdPerson && (userTargetDistance <= MIN_DISTANCE + 0.15f); }
    [[nodiscard]] bool isGrounded() const { return grounded; }
    [[nodiscard]] bool isJumping() const { return !grounded && verticalVelocity > 0.0f; }
    // A swimming character is not grounded and is not rising, but it is not
    // falling either - treating it as falling made leaving the water register as
    // a hard landing.
    [[nodiscard]] bool isFalling() const { return !grounded && !swimming && verticalVelocity <= 0.0f; }
    /// How far the drawn character (and the camera pivot) sits from the
    /// physics position in Z this frame. A grounded step up or down moves the
    /// physics feet in one frame; the picture eases after them over about a
    /// tenth of a second, as the stock client's does. Zero at rest.
    [[nodiscard]] float getRenderZOffset() const { return renderZOffset_; }

    // Call every frame while riding a transport that forces Z to a locked value
    // externally (e.g. the Deeprun Tram, which has no real floor mid-tunnel).
    // Without this, gravity keeps integrating verticalVelocity every frame since
    // grounded never goes true on a moving M2 deck, even though the position
    // itself is being overwritten and looks static - the huge fall speed that
    // silently built up over the whole ride would then unleash all at once the
    // instant the external Z lock releases at disembark, clipping the player
    // through the floor.
    void suppressVerticalPhysics() { verticalVelocity = 0.0f; grounded = true; }
    [[nodiscard]] bool isJumpKeyPressed() const { return jumpBufferTimer > 0.0f; }
    /// The ground speed the last update moved at (CMovement::GetSpeed for the
    /// keys held), 0 standing: a mount's pace and playback rate follow it.
    [[nodiscard]] float currentMoveSpeed() const { return currentMoveSpeed_; }
    [[nodiscard]] float currentWalkSpeed() const { return stockSpeeds().walk; }
    [[nodiscard]] bool isSprinting() const;
    [[nodiscard]] bool isMovingForward() const { return moveForwardActive; }
    [[nodiscard]] bool isMovingBackward() const { return moveBackwardActive; }
    [[nodiscard]] bool isStrafingLeft() const { return strafeLeftActive; }
    [[nodiscard]] bool isStrafingRight() const { return strafeRightActive; }
    [[nodiscard]] bool isTurningLeft() const { return turningLeftActive; }
    [[nodiscard]] bool isTurningRight() const { return turningRightActive; }
    [[nodiscard]] bool isAutoRunning() const { return autoRunning; }
    [[nodiscard]] bool isRightMouseHeld() const { return rightMouseDown; }
    [[nodiscard]] bool isSitting() const { return sitting; }
    [[nodiscard]] bool isSwimming() const { return swimming; }
    [[nodiscard]] bool isInsideWMO() const { return cachedInsideWMO; }
    [[nodiscard]] bool isInsideInteriorWMO() const { return cachedInsideInteriorWMO; }
    void setGrounded(bool g) { grounded = g; }
    void setSitting(bool s) { sitting = s; }
    /// What the server says the character is doing: standing, sitting, seated
    /// in a chair, asleep, kneeling. UnitStandStateType.
    ///
    /// Sitting in something is a position this client does not compute - the
    /// server put the character in the chair - so grounding stands down for it.
    ///
    /// Both flags, from the one number: 1-6 is sitting or asleep and 8 is
    /// kneeling, all of which movement cancels, and 2 and 4-6 are the seats a
    /// chair puts a character in.
    void applyServerStandState(uint8_t standState) {
        sitting = (standState >= 1 && standState <= 6) || standState == 8;
        const bool wasSeated = seatedInChair_;
        seatedInChair_ = standState == 2 || (standState >= 4 && standState <= 6);
        // The seat is where the server's teleport put the character, not
        // wherever grounding drifted in between. The two packets are sent
        // together but processed under a per-frame budget, so a frame can run
        // between them with the character still standing - and in that frame
        // grounding steps the feet up onto the chair's collision mesh. Seated
        // grounding never lowers, so the character then sat in the air above
        // the chair for as long as they stayed in it.
        if (seatedInChair_ && !wasSeated && followTarget &&
            serverPlacementAge_ < kServerPlacementWindow) {
            const glm::vec2 d = glm::vec2(*followTarget) - glm::vec2(serverPlacement_);
            if (glm::dot(d, d) < 0.25f) {
                *followTarget = serverPlacement_;
                lastGroundZ = serverPlacement_.z;
                verticalVelocity = 0.0f;
                grounded = true;
            }
        }
    }
    /// Where the server last placed the character (a same-map teleport), in
    /// render coordinates. Read back by applyServerStandState for a seat.
    void noteServerPlacement(const glm::vec3& renderPos) {
        serverPlacement_ = renderPos;
        serverPlacementAge_ = 0.0f;
    }
    [[nodiscard]] bool isSeatedInChair() const { return seatedInChair_; }

    /// Look the character in the face, close in, for the barber shop.
    ///
    /// The barber's preview is the character themselves - the interface asks
    /// for no camera at all, because on the real client this is the client's
    /// job. Without it the camera stays where it was, behind the character or
    /// pointed at a wall, and cycling styles changes something nobody can see:
    /// reported, twice, as there being no preview.
    ///
    /// The player's own view is put back when the chair is left.
    void setBarberShopView(bool on) {
        if (on == barberView_) return;
        barberView_ = on;
        // A collision recovery in progress belongs to the old view, and a
        // follow ease must not carry the camera off the face it is set to.
        collisionShownDist_ = -1.0f;
        followYaw_.active = false;
        followPitch_.active = false;
        if (on) {
            savedBarberYaw_ = yaw;
            savedBarberPitch_ = pitch;
            savedBarberDistance_ = userTargetDistance;
            savedBarberPivot_ = pivotHeight_;
            // Round to the front: the camera yaw is the character's facing, so
            // half a turn from it is looking back at them.
            yaw = facingYaw + 180.0f;
            // Slightly above level, so the face is in view rather than the top
            // of the head, and close enough for hair to be worth choosing.
            pitch = 8.0f;
            userTargetDistance = 2.4f;
            pivotHeight_ = 1.7f;
        } else {
            yaw = savedBarberYaw_;
            pitch = savedBarberPitch_;
            userTargetDistance = savedBarberDistance_;
            pivotHeight_ = savedBarberPivot_;
        }
    }
    [[nodiscard]] bool isBarberShopView() const { return barberView_; }
    /// Ignore the slope limit, so any face can be walked straight up.
    ///
    /// For reaching a place to look at it, not for playing. Off by default and
    /// never saved: it is a thing you turn on for a minute.
    void setIgnoreSlopeLimit(bool ignore) { ignoreSlopeLimit_ = ignore; }
    [[nodiscard]] bool ignoresSlopeLimit() const { return ignoreSlopeLimit_; }
    [[nodiscard]] bool isOnTaxi() const { return externalFollow_; }
    [[nodiscard]] const glm::vec3* getFollowTarget() const { return followTarget; }
    glm::vec3* getFollowTargetMutable() { return followTarget; }

    // Movement callback for sending opcodes to server
    using MovementCallback = std::function<void(uint32_t opcode)>;
    void setMovementCallback(MovementCallback cb) { movementCallback = std::move(cb); }

    // Callback invoked when the player stands up via local input (space/X/movement key
    // while server-sitting), so the caller can send CMSG_STAND_STATE_CHANGE(0).
    using StandUpCallback = std::function<void()>;
    void setStandUpCallback(StandUpCallback cb) { standUpCallback_ = std::move(cb); }

    // Callback invoked when the player sits down via local input (X key).
    using SitDownCallback = std::function<void()>;
    void setSitDownCallback(SitDownCallback cb) { sitDownCallback_ = std::move(cb); }

    // Callback invoked when auto-follow is cancelled by user movement input.
    using AutoFollowCancelCallback = std::function<void()>;
    void setAutoFollowCancelCallback(AutoFollowCancelCallback cb) { autoFollowCancelCallback_ = std::move(cb); }

    void setUseWoWSpeed(bool use) { useWoWSpeed = use; }
    void setRunSpeedOverride(float speed) { runSpeedOverride_ = speed; }
    void setWalkSpeedOverride(float speed) { walkSpeedOverride_ = speed; }
    void setSwimSpeedOverride(float speed) { swimSpeedOverride_ = speed; }
    void setSwimBackSpeedOverride(float speed) { swimBackSpeedOverride_ = speed; }
    void setFlightSpeedOverride(float speed) { flightSpeedOverride_ = speed; }
    void setFlightBackSpeedOverride(float speed) { flightBackSpeedOverride_ = speed; }
    void setRunBackSpeedOverride(float speed) { runBackSpeedOverride_ = speed; }
    // Server turn rate in rad/s (SMSG_FORCE_TURN_RATE_CHANGE); 0 = use WOW_TURN_SPEED default
    void setTurnRateOverride(float rateRadS) { turnRateOverride_ = rateRadS; }
    void setMovementRooted(bool rooted) { movementRooted_ = rooted; }
    /// A driven vehicle's MOVEMENTFLAG2_NO_STRAFE (0x1) / NO_JUMPING (0x2)
    /// (Vehicle.flags 0x1 / 0x2): the strafe keys or the jump do nothing.
    void setMoveRestrictions(bool noStrafe, bool noJump) { noStrafe_ = noStrafe; noJump_ = noJump; }
    /// Moves the point the camera orbits off the followed position: a
    /// vehicle seat's CameraOffset, or its seat point (VehicleCamera_C
    /// 0x759200). Zero on foot.
    void setPivotOffset(const glm::vec3& offset) { pivotOffset_ = offset; }
    /// How far the camera sits from the point it looks at (the zoom, less
    /// any collision pull-in).
    [[nodiscard]] float getViewDistance() const { return std::min(currentDistance, collisionDistance); }
    [[nodiscard]] bool isMovementRooted() const { return movementRooted_; }
    void setGravityDisabled(bool disabled) { gravityDisabled_ = disabled; }
    void setFeatherFallActive(bool active) { featherFallActive_ = active; }
    void setWaterWalkActive(bool active) { waterWalkActive_ = active; }
    /// The player's collision box (stock 0x6E9570): width and height from the
    /// display model, and the step height. Drives the wall radius, the step-up
    /// reach and the swim depths.
    void setPlayerCollider(const game::stock_movement::Collider& c) { collider_ = c; }
    [[nodiscard]] const game::stock_movement::Collider& getPlayerCollider() const { return collider_; }
    /// Permission to fly - a flying mount where flight is allowed, or .gm fly.
    /// Not the same as being in the air: see isFlightAirborne.
    void setFlyingActive(bool active);
    [[nodiscard]] bool isFlyingActive() const { return flyingActive_; }
    /// In the air on it. Space takes off, and touching real ground without
    /// climbing lands - after which the mount runs, falls and jumps like any
    /// other until it takes off again. Flight physics, the flight animations
    /// and the FLYING movement flag all follow this rather than permission.
    [[nodiscard]] bool isFlightAirborne() const { return flyingActive_ && flightAirborne_; }
    /// The pitch the player is flying at, in radians: the camera's while it
    /// steers, and held when it does not. What the mount tilts to and what
    /// the server is told.
    [[nodiscard]] float getFlightPitchRad() const { return glm::radians(flightPitchDeg_); }
    [[nodiscard]] bool isAscending() const { return wasAscending_; }
    [[nodiscard]] bool isDescending() const { return wasDescending_; }
    void setHoverActive(bool active) { hoverActive_ = active; }
    void setMounted(bool m) { mounted_ = m; }
    void setIntoxication(float amount) { intoxication_ = std::clamp(amount, 0.0f, 1.0f); }
    /// The stock drunk level, PLAYER_BYTES_3 byte 1 / 100 (0x4F7290): what the
    /// stock steer reads. setIntoxication is Wowee's own, from the server's
    /// threshold message, and drives the Drunken wobble enhancement.
    void setDrunkLevel(float level) { drunkLevel_ = std::clamp(level, 0.0f, 1.0f); }
    /// The Drunken wobble enhancement: Wowee's camera sway and travel weave
    /// in place of the stock facing steer.
    void setDrunkWobble(bool on) { drunkWobble_ = on; }
    void setMountHeightOffset(float offset) { mountHeightOffset_ = offset; }
    void setExternalFollow(bool enabled) { externalFollow_ = enabled; }
    void setExternalMoving(bool moving) { externalMoving_ = moving; }
    void setFacingYaw(float yaw) { facingYaw = yaw; }  // For taxi/scripted movement
    void clearMovementInputs();
    void suppressMovementFor(float seconds) { movementSuppressTimer_ = seconds; }
    void suspendGravityFor(float seconds) { gravitySuspendTimer_ = seconds; }

    /// Where the server says the player is, on entering a world.
    ///
    /// Nothing under them has streamed in yet, so for a moment the heightfield
    /// is the only floor there is - and over a WMO that runs underground it
    /// lies well above where they actually stand. Logging out in the Undercity
    /// entrance tunnel and back in put the player on its roof. Until the
    /// buildings arrive, no floor far above the position the server gave is
    /// their floor.
    void setEntryFloorAnchor(float renderZ, float seconds) {
        entryFloorAnchorZ_ = renderZ;
        entryFloorAnchorTimer_ = seconds;
    }

    // Auto-follow: walk toward a target position each frame (WoW /follow).
    // The caller updates *targetPos every frame with the followed entity's render position.
    // Stops within FOLLOW_STOP_DIST; cancels on manual WASD input.
    void setAutoFollow(const glm::vec3* targetPos) { autoFollowTarget_ = targetPos; }
    void cancelAutoFollow() { autoFollowTarget_ = nullptr; }
    [[nodiscard]] bool isAutoFollowing() const { return autoFollowTarget_ != nullptr; }

    // Apply server-driven knockback impulse.
    // dir: render-space 2D direction unit vector (from vcos/vsin in packet)
    // hspeed: horizontal speed magnitude (units/s)
    // vspeed: raw packet vspeed field (server sends negative for upward launch)
    void applyKnockBack(float vcos, float vsin, float hspeed, float vspeed);

    // Trigger a camera shake effect (e.g. from SMSG_CAMERA_SHAKE).
    // magnitude: peak positional offset in world units
    // frequency: oscillation frequency in Hz
    // duration: shake duration in seconds
    void triggerShake(float magnitude, float frequency, float duration);
    /// One CameraShakes.dbc row, as the stock camera keeps it (Wow.exe
    /// 0x606330): type 0 a steady sine, 1 a damped one (exp(-t*coefficient));
    /// direction 0 along the player's facing, 1 across it, 2 up and down;
    /// amplitude in the row's units (1/36 yard); frequency and duration in
    /// Hz and seconds; phase in seconds. source is where it went off: full
    /// within 9 yards of the player, 0.7^((d-9)/9) out to 80 (0x6004B0).
    void addStockShake(int type, int direction, float amplitude, float frequency,
                       float duration, float phase, float coefficient, const glm::vec3& source);
    /// For the debug Lua hook: put the camera at a yaw (degrees, relative to
    /// the character's facing), pitch and distance.
    void debugSetView(float yawOffsetDeg, float pitchDeg, float distance);

    /// How much of a shake to actually apply, 0 to 1.
    ///
    /// The client moves the view on its own for spell effects, for
    /// thunderstorms and for drunkenness, and had no control over any of it.
    /// Nothing in 2004 offered one; every game does now, because for some
    /// people it is the difference between playing and feeling ill. Zero stops
    /// it outright - triggerShake drops a magnitude of zero on the floor.
    ///
    /// It does not touch the weave a drunk character walks with. That is what
    /// being drunk does to the character rather than to the picture, and taking
    /// it away would be an advantage rather than a comfort.
    void setShakeScale(float scale) {
        shakeScale_ = scale < 0.0f ? 0.0f : (scale > 1.0f ? 1.0f : scale);
    }
    [[nodiscard]] float shakeScale() const { return shakeScale_; }

    // For first-person player hiding
    void setCharacterRenderer(class CharacterRenderer* cr, uint32_t playerId) {
        characterRenderer = cr;
        // Another model (a shapeshift, a polymorph, turning back): its head
        // bone is measured afresh. The kept answer is for a frame where the
        // same model cannot answer, not for a different model - a cat form
        // with a head bone at 0.6 would otherwise orbit the night elf's 1.8,
        // and a model without one would never drop to its own height.
        if (playerId != playerInstanceId) {
            followedHeight_ = 0.0f;
            followedHeadZ_ = 0.0f;
        }
        playerInstanceId = playerId;
    }

private:
    Camera* camera;
    TerrainManager* terrainManager = nullptr;
    /// One frame's input and physics setup, handed to whichever camera mode is
    /// running.
    ///
    /// update() reads the keyboard, works out the speed and the axes, and then
    /// splits into two branches - the third-person camera that moves a character
    /// and the free-fly camera that moves itself. The branches were fifteen
    /// hundred lines and two hundred lines of one function, and this is what
    /// they both need from the part before the split.
    struct FrameInput {
        float physicsDeltaTime = 0.0f;
        float gravity = 0.0f;
        float jumpVel = 0.0f;
        float speed = 0.0f;
        glm::vec3 forward{0.0f};      ///< movement axes, flattened onto XY
        glm::vec3 right{0.0f};
        glm::vec3 forward3D{0.0f};    ///< the camera's own forward, with pitch
        glm::vec3 flightForward{0.0f};   ///< where W flies: the facing, at the flight pitch
        glm::vec3 movement{0.0f};     ///< the horizontal move this frame; the modes adjust it
        bool nowForward = false;
        bool nowBackward = false;
        bool nowStrafeLeft = false;
        bool nowStrafeRight = false;
        bool nowTurnLeft = false;
        bool nowTurnRight = false;
        bool nowJump = false;
        bool spaceHeld = false;       ///< held, not pressed: rising in water and in flight
        bool xDown = false;
        bool uiWantsKeyboard = false;
    };

    /// The camera that orbits a character and moves it: collision, grounding,
    /// swimming, zoom, and the pullback that keeps the camera out of walls.
    void updateThirdPersonCamera(float deltaTime, FrameInput& f);

    /// The camera that flies itself, used when nothing is being followed.
    void updateFreeFlyCamera(float deltaTime, FrameInput& f);

    /// Where the followed character wants to be this frame: the movement
    /// intent and the swept wall collision. The floor is not consulted yet.
    glm::vec3 moveFollowedCharacter(float deltaTime, FrameInput& f, glm::vec3& prevTargetPos);

    /// What the floor queries answer directly under the character: the floor
    /// that was chosen, and the three surfaces that were offered. The recovery
    /// probes need to know which existed at all, not just which one won.
    struct FloorSample {
        std::optional<float> floor;
        std::optional<float> terrain;
        std::optional<float> wmo;
        std::optional<float> m2;
    };

    /// Ask terrain, WMO and doodads where the floor is - two of them on other
    /// threads - or answer from the cache when the character has barely moved.
    FloorSample sampleFloorUnderFeet(const glm::vec3& targetPos, float stepUpBudget);

    /// Put the character on the floor and publish where it ended up - the
    /// expensive half, with the floor queries and the cache that skips them.
    void groundFollowedCharacter(float deltaTime, FrameInput& f, glm::vec3& targetPos,
                                 const glm::vec3& prevTargetPos);

    /// Where the camera sits relative to the character it follows: pivot, zoom,
    /// the pullback that keeps it out of walls, and the first-person threshold.
    void updateOrbitCamera(float deltaTime, FrameInput& f, const glm::vec3& targetPos);

    /// Move from `from` to `to` in small steps, letting the walls push back.
    ///
    /// A single long move tunnels through thin geometry, so it is broken into
    /// steps of 20cm inside a building and 35cm outside, capped at eight. An
    /// upward Z correction is kept - that is a ramp - and a downward one is
    /// dropped, because a wall must never pull the character into the floor.
    ///
    /// Written out three times, for walking, for swimming, and for the free-fly
    /// camera, with the step sizes and the cap repeated in each.
    /// `includeDoodads` adds M2 collision, which the free-fly camera does not use.
    glm::vec3 sweepAgainstWalls(const glm::vec3& from, const glm::vec3& to, bool includeDoodads);

    WMORenderer* wmoRenderer = nullptr;
    M2Renderer* m2Renderer = nullptr;
    WaterRenderer* waterRenderer = nullptr;
    CharacterRenderer* characterRenderer = nullptr;
    uint32_t playerInstanceId = 0;

    // Stored rotation (avoids lossy forward-vector round-trip)
    float yaw = 180.0f;
    float pitch = -30.0f;
    float facingYaw = 180.0f;  // Character-facing yaw (can differ from camera yaw)
    float travelYaw_ = 180.0f; // Heading of actual movement vector (see getTravelYaw)

    // Movement settings
    float movementSpeed = 50.0f;
    float sprintMultiplier = 3.0f;
    float slowMultiplier = 0.3f;

    // Mouse settings
    float mouseSensitivity = 0.2f;
    bool invertMouse = false;
    bool mouseButtonDown = false;
    bool leftMouseDown = false;
    bool rightMouseDown = false;
    // Camera rotation only begins once the mouse moves past a small dead-zone while a
    // button is held. This keeps a click (to select an NPC) from nudging the view on
    // tiny hand jitter - which made NPCs appear to shift out from under the cursor.
    bool rotateArmed_ = false;
    /// Where the cursor has got to since the press, as a net offset.
    ///
    /// Summed signed rather than as |dx|+|dy| per event: the old measure was
    /// the length of the path the cursor took, so a trackpad click that
    /// jittered a few pixels and came back to where it started still read as
    /// a drag and swung the view. What decides a click elsewhere is the
    /// straight line from press to release, and this is now the same line.
    float dragOffsetX_ = 0.0f;
    float dragOffsetY_ = 0.0f;

    // Third-person orbit camera (WoW-style)
    bool thirdPerson = false;
    // Stock starts at the default view's distance: cameraView "2" picks view
    // B, cameraDistanceB "5.55" / cameraPitchB "10.0" (table at 0xad1ba8,
    // CGCamera ctor FUN_00606b30 loads it into +0x118/+0x120).
    float userTargetDistance = 5.55f;   // What the player wants (scroll wheel) - CGCamera +0x1e8
    float currentDistance = 5.55f;      // Zoom-driven distance - CGCamera +0x118
    float collisionDistance = 5.55f;    // Max allowed by collision
    bool externalFollow_ = false;
    static constexpr float MIN_DISTANCE = 0.5f;     // Minimum zoom (first-person threshold)
    /// cameraDistanceMax default "15.0" (Wow.exe 12340, registered in
    /// FUN_005fd910 @ 0x5fe192); times cameraDistanceMaxFactor (default 1.0,
    /// Interface Options slider 1..2) is the zoom limit (FUN_006000e0).
    static constexpr float MAX_DISTANCE_NORMAL = 15.0f;
    /// The hard cap: FUN_006000e0 clamps max * factor to 50.0 (0xa1e2fc), and
    /// FUN_005fd680 refuses a cameraDistanceMax above it.
    static constexpr float MAX_DISTANCE_EXTENDED = 50.0f;
public:
    /// The largest multiple worth offering: the camera as far back as this
    /// client has ever allowed. Declared here because it is built from the two
    /// constants above. See setMaxDistanceFactor.
    static constexpr float kMaxDistanceFactorLimit =
        MAX_DISTANCE_EXTENDED / MAX_DISTANCE_NORMAL;
private:
    // There is no indoor zoom limit: the stock CGCamera has one maximum
    // (FUN_006000e0: min(cameraDistanceMax * factor, 50), or a vehicle's own
    // at +0x2cc) and leaves interiors to the collision pull-in below. The
    // 6-yard interior cap this client used to apply is gone.
    bool extendedZoom_ = false;
    /// Zoom travel, yards a second. cameraDistanceMoveSpeed "8.33" (0xa1e7e0,
    /// registered @ 0x5fda40). FUN_005ff950/005ffa60 turn a zoom request of
    /// N yards into N / 8.33 s of travel (accumulated if one is already
    /// running) and FUN_006000e0 moves the distance linearly over it.
    static constexpr float kZoomMoveSpeed = 8.33f;
    /// One wheel notch: Bindings.xml CAMERAZOOMIN/OUT run CameraZoomIn(1.0) /
    /// CameraZoomOut(1.0) (FUN_006017e0 / FUN_00601840).
    static constexpr float kZoomStepYards = 1.0f;
    /// Moves currentDistance toward userTargetDistance at kZoomMoveSpeed.
    void stepZoomTowardTarget(float deltaTime);

    // ---- Stock collision pull-in (FUN_00606f90 @ 0x60789x / FUN_00605d60) ----
    /// The distance the camera is allowed out to after a collision: the stock
    /// +0x118 while its smoothing flag 0x4000000 is up. A hit nearer than it
    /// by more than kCollisionSnap snaps it in to hit + kCollisionSnapOffset
    /// at once, and it then eases back out to the zoom distance over
    /// kCollisionRecoverSeconds with the cosine curve; the camera sits at
    /// min(hit, this). Negative when no pull-in is active.
    float collisionShownDist_ = -1.0f;
    float collisionEaseFrom_ = 0.0f;
    float collisionEaseElapsed_ = 0.0f;
    static constexpr float kCollisionSnap = 0.1111111f;        // 0xa1e354
    static constexpr float kCollisionSnapOffset = 0.1111121f;  // 0xa1ea80
    static constexpr float kCollisionRecoverSeconds = 2.0f;    // 0xa4040c

    // ---- Stock camera following (cameraSmoothStyle) ----
    /// One eased value, as the stock smoothing slots hold them (yaw at
    /// CGCamera +0x258.., pitch at +0x228..): from, to, a duration, and the
    /// cosine curve FUN_008ca080 lerp(a, b, (1 - cos(pi t)) / 2), evaluated
    /// each frame by FUN_00603d30.
    struct FollowEase {
        bool active = false;
        float from = 0.0f;
        float to = 0.0f;
        float duration = 0.0f;
        float elapsed = 0.0f;
    };
    FollowEase followYaw_;    ///< eases the camera's yaw offset from the facing
    FollowEase followPitch_;  ///< eases the pitch into the band below
    /// Dropdown index: 0 Never, 1 Only horizontal when moving (cameraSmoothStyle
    /// 1, "Smart"), 2 Only when moving (4, "Smarter", the stock default),
    /// 3 Always (2).
    int followStyle_ = 2;
    /// The movement keys and mouse buttons last seen, packed. The stock client
    /// re-evaluates following (FUN_00602760) when these change - from
    /// CGInputControl's key down/up handlers FUN_005fa170 / FUN_005fa450 -
    /// not every frame.
    uint32_t followInputSig_ = 0xFFFFFFFFu;
    /// cameraYawSmoothSpeed "180", cameraPitchSmoothSpeed "45" (deg/s),
    /// cameraPitchSmoothMin "0.0" / Max "30.0" (deg, positive = looking
    /// down), cameraSmoothTimeMin "0.1" / Max "2.0" (s). FUN_005fd910.
    static constexpr float kFollowYawSpeedDeg = 180.0f;
    static constexpr float kFollowPitchSpeedDeg = 45.0f;
    static constexpr float kFollowPitchDownMinDeg = 0.0f;
    static constexpr float kFollowPitchDownMaxDeg = 30.0f;
    static constexpr float kFollowTimeMin = 0.1f;
    static constexpr float kFollowTimeMax = 2.0f;
    void updateStockFollow(float deltaTime, bool forward, bool backward,
                           bool strafeLeft, bool strafeRight,
                           bool turnLeft, bool turnRight, bool cameraDrivesFacing);
public:
    /// Camera Following Style. See followStyle_.
    void setCameraFollowStyle(int style) {
        followStyle_ = std::clamp(style, 0, 3);
        followInputSig_ = 0xFFFFFFFFu;  // re-evaluate on the next frame
    }
    [[nodiscard]] int cameraFollowStyle() const { return followStyle_; }
private:
    static constexpr float CAM_SMOOTH_SPEED_DEFAULT = 30.0f;
    float camSmoothSpeed_ = CAM_SMOOTH_SPEED_DEFAULT;  // User-configurable camera smoothing (higher = tighter)
    // When true the camera keeps its exponential lerp even while actively
    // dragging or keyboard-turning - the pre-snap "floaty follow" feel.
    // When false (default), rotation input moves the camera 1:1.
    bool smoothCameraFollow_ = false;
public:
    void setCameraSmoothSpeed(float speed) { camSmoothSpeed_ = std::clamp(speed, 5.0f, 100.0f); }
    [[nodiscard]] float getCameraSmoothSpeed() const { return camSmoothSpeed_; }
    void setSmoothCameraFollow(bool smooth) { smoothCameraFollow_ = smooth; }
    [[nodiscard]] bool isSmoothCameraFollow() const { return smoothCameraFollow_; }
    void setPivotHeight(float h) { pivotHeight_ = std::clamp(h, 0.0f, 3.0f); }
    [[nodiscard]] float getPivotHeight() const { return pivotHeight_; }

    /// Where the camera looks, on the character it is following.
    ///
    /// pivotHeight_ is a distance in metres, and a metre is a different part
    /// of a gnome than it is of a tauren: at a fixed 1.6 the camera orbited a
    /// point above a short character's head and zoomed into thin air.
    ///
    /// Scaling it by total height was not enough. Measured against the head
    /// bone afterwards, it put the pivot between 81% and 92% of the way up to
    /// the head for every race in the game - and at 126% for a gnome female
    /// and 132% for a gnome male, a quarter of a metre above the bone, on a
    /// character 1.3 tall. A gnome's head and hair are 40% of its height
    /// where everyone else's are 15%, so its box says nothing about where its
    /// face is. The setting is read as a proportion of the head bone it was
    /// chosen against and applied to the head bone of whoever is being
    /// followed. Height stays as the fallback, for a skeleton with no head
    /// bone in it.
    ///
    /// Static and given the setting, so the arithmetic can be checked against
    /// the measured models without standing a camera up.
    [[nodiscard]] static float pivotHeightFor(float setting, float characterHeight,
                                              float headBoneZ) {
        if (headBoneZ > 0.01f) {
            return std::clamp(headBoneZ * (setting / kPivotReferenceHeadZ), 0.2f, 3.0f);
        }
        if (characterHeight <= 0.01f) return setting;
        return std::clamp(characterHeight * (setting / kPivotReferenceHeight), 0.2f, 3.0f);
    }

    /// How far the pivot has to rise for the camera to clear the ground.
    ///
    /// The camera sits behind the character along the look direction, so on a
    /// slope it can end up inside the hill; raising the pivot raises it with
    /// it. All four arguments are in world Z except the direction, which is
    /// the vertical part of the unit vector from pivot to camera.
    ///
    /// Measured at the camera rather than at the pivot, and against a small
    /// clearance rather than a fixed height above the terrain. The rule was
    /// "keep the pivot two metres above the ground under the camera", which
    /// is an absolute height and therefore undoes any attempt to put the
    /// pivot on the character by exactly the amount that attempt moved it: on
    /// flat ground a gnome's 0.65 was lifted by 1.35 and a human's 1.60 by
    /// 0.40, and both ended up at 2.00 above the feet. It made no difference
    /// which character was being followed, which is what made three separate
    /// fixes to the pivot itself change nothing on screen.
    ///
    /// Static and given everything it uses, so the rule can be checked
    /// against a gnome on flat ground without standing a world up.
    [[nodiscard]] static float terrainPivotLift(float pivotZ, float camDirZ, float distance,
                                                float terrainZAtCamera) {
        constexpr float kMinCamClearance = 0.6f;
        constexpr float kMaxLift = 1.4f;
        const float camZ = pivotZ + camDirZ * distance;
        const float clearance = camZ - terrainZAtCamera;
        if (clearance >= kMinCamClearance) return 0.0f;
        return std::clamp(kMinCamClearance - clearance, 0.0f, kMaxLift);
    }

    /// The pivot for whoever is being followed right now.
    [[nodiscard]] float followedPivotHeight() const {
        return pivotHeightFor(pivotHeight_, followedHeight_, followedHeadZ_);
    }
private:
    static constexpr float PIVOT_HEIGHT_DEFAULT = 1.6f;
    /// The height PIVOT_HEIGHT_DEFAULT was chosen against, so the default puts
    /// the pivot exactly where it always did for a character of that size and
    /// only moves it for the ones it was wrong for.
    ///
    /// Measured off the models rather than guessed: a human male stands 2.13
    /// in bind pose, a night elf 2.29, a tauren 2.33, a gnome 1.17. Guessed at
    /// two metres this put every pivot at 80% of height - which lowered a
    /// gnome's, the point of the change, but quietly raised a night elf's from
    /// 1.60 to 1.83. Against the real figure the tall races keep what they had.
    static constexpr float kPivotReferenceHeight = 2.13f;
    /// Where the head bone sits on the character the default was chosen
    /// against - a human male's is at 1.843 - so that camera stays exactly
    /// where it was and only the ones the metre was wrong for move. Against
    /// it a night elf female goes 1.72 -> 1.77, a tauren male 1.75 -> 1.64,
    /// and a gnome male 1.00 -> 0.65.
    static constexpr float kPivotReferenceHeadZ = 1.843f;
    float pivotHeight_ = PIVOT_HEIGHT_DEFAULT;  // User-configurable pivot height
    /// The followed character's height, refreshed as it is followed. Zero
    /// until something has been measured, which reads as the old behaviour.
    float followedHeight_ = 0.0f;
    /// The followed character's head bone in model Z, zero until measured or
    /// if its skeleton names no head.
    float followedHeadZ_ = 0.0f;
    /// Whether the terrain is currently raising the pivot, so that starting
    /// and stopping is said once each rather than every frame.
    bool pivotLiftActive_ = false;
    static constexpr float CAM_SPHERE_RADIUS = 0.32f;  // Keep camera farther from geometry to avoid clipping-through surfaces
    static constexpr float CAM_EPSILON = 0.22f;        // Extra wall offset to avoid near-plane clipping artifacts
    static constexpr float COLLISION_FOCUS_RADIUS_THIRD_PERSON = 20.0f;  // Reduced for performance
    static constexpr float COLLISION_FOCUS_RADIUS_FREE_FLY = 20.0f;
    // Stock pitch limits: +/-1.5533431 rad = 89 deg (0xad1b4c / 0xad1b50),
    // clamped in FUN_006000e0, FUN_005ffc20 (mouse) and FUN_005ff530.
    static constexpr float MIN_PITCH = -89.0f;      // Look almost straight down
    static constexpr float MAX_PITCH = 89.0f;       // Look almost straight up
    glm::vec3* followTarget = nullptr;
    glm::vec3 smoothedCamPos = glm::vec3(0.0f);     // For smooth camera movement
    // (The asymmetric collision smoothing and the sideways step around
    // obstructions are gone: the stock pull-in is collisionShownDist_.)
    /// How far the camera is currently being lifted clear of, or dropped
    /// below, the water surface. Eased both ways so crossing the water's
    /// edge does not jump the view.
    float waterNudgeZ_ = 0.0f;

    // Gravity / grounding
    float verticalVelocity = 0.0f;
    bool grounded = false;
    // Movement keys as they were on leaving the ground; see the latch in update().
    bool airMoveLatched_ = false;
    bool airForward_ = false;
    bool airBackward_ = false;
    bool airStrafeLeft_ = false;
    bool airStrafeRight_ = false;
    /// The world direction and speed the fall goes at, fixed when the ground
    /// is left (CMovement +0x64/+0x8C are not recomputed while FALLING: see
    /// 0x987EF0 / 0x9880C0). Turning in the air turns the character, not the
    /// path. Zero speed for a standing jump until a direction key is pressed in
    /// the air, which starts a drift at walk speed (0x988A20 / 0x988B00 force
    /// the recompute with GetSpeed(walk)) - once per fall.
    float airDirX_ = 0.0f;
    float airDirY_ = 0.0f;
    float airSpeed_ = 0.0f;
    /// Visual Z lag behind the physics feet after a grounded step (negative
    /// while the picture is still below a step just taken up). Decays to zero
    /// with RENDER_Z_SMOOTH_TAU; never applied to a landing, which is exact.
    float renderZOffset_ = 0.0f;
    static constexpr float RENDER_Z_SMOOTH_TAU = 0.035f;  // ~95% settled in 0.1 s
    /// Airborne, how far above the feet a surface under the character's
    /// centre is looked for: a ledge top this close overhead is a ledge side
    /// to be stopped by, not a floor to be lifted onto. Matches the wall
    /// tests' jump clearance (1.2), which leave such ledges to the floor pass.
    static constexpr float AIR_LEDGE_REACH = 1.2f;
    /// Standing on ground steeper than the walkable limit: the horizontal
    /// velocity it has given the character down the slope, and whether the
    /// character is on such ground now (no jumping from it, as stock: the
    /// client treats that slide as a fall). See groundFollowedCharacter.
    glm::vec2 slopeSlideVel_{0.0f};
    bool onSteepSlope_ = false;
    static constexpr float SLOPE_SLIDE_MAX_SPEED = 25.0f;
    static constexpr float STAND_EYE_HEIGHT = 1.2f;  // Standing eye height
    static constexpr float CROUCH_EYE_HEIGHT = 0.6f; // Crouching eye height
    float eyeHeight = STAND_EYE_HEIGHT;
    float lastGroundZ = 0.0f;  // Last known ground height (fallback when no terrain)
    glm::vec3 lastGroundedPos_{0.0f};  // Last position that had ground under it (void recovery)
    bool hasLastGroundedPos_ = false;
    static constexpr float GRAVITY = -30.0f;
    static constexpr float JUMP_VELOCITY = 15.0f;
    float jumpBufferTimer = 0.0f;   // Time since space was pressed
    float coyoteTimer = 0.0f;       // Time since last grounded
    static constexpr float JUMP_BUFFER_TIME = 0.15f;  // 150ms input buffer
    // No grace after leaving the ground: the stock client cannot jump once
    // airborne. The jump buffer below still fires a press made a moment
    // before the feet find ground again.
    static constexpr float COYOTE_TIME = 0.0f;

    // Cached isInsideWMO result (throttled to avoid per-frame cost)
    bool cachedInsideWMO = false;
    bool cachedInsideInteriorWMO = false;
    int insideStateCheckCounter_ = 0;
    glm::vec3 lastInsideStateCheckPos_ = glm::vec3(0.0f);
    int insideWMOCheckCounter = 0;
    glm::vec3 lastInsideWMOCheckPos = glm::vec3(0.0f);

    // Cached camera WMO floor query (skip if camera moved < 0.3 units)
    glm::vec3 lastCamFloorQueryPos = glm::vec3(0.0f);
    std::optional<float> cachedCamWmoFloor;
    bool hasCachedCamFloor = false;

    // Terrain-aware camera pivot lift cache (throttled for performance).
    glm::vec3 lastPivotLiftQueryPos_ = glm::vec3(0.0f);
    float lastPivotLiftDistance_ = 0.0f;
    int pivotLiftQueryCounter_ = 0;
    float cachedPivotLift_ = 0.0f;
    static constexpr int PIVOT_LIFT_QUERY_INTERVAL = 3;
    static constexpr float PIVOT_LIFT_POS_THRESHOLD = 0.5f;
    static constexpr float PIVOT_LIFT_DIST_THRESHOLD = 0.5f;

    // Cached floor height queries (update every 5 frames or 2 unit movement)
    glm::vec3 lastFloorQueryPos = glm::vec3(0.0f);
    std::optional<float> cachedFloorHeight;
    int floorQueryFrameCounter = 0;
    static constexpr float FLOOR_QUERY_DISTANCE_THRESHOLD = 2.0f;  // Increased from 1.0
    static constexpr int FLOOR_QUERY_FRAME_INTERVAL = 5;  // Increased from 3


    // Ray-march the terrain heightfield along pivot→camDir and return the
    // farthest camera distance that keeps clearance above the terrain surface.
    // Returns maxDist when the ray stays clear (or terrain is unavailable).
    [[nodiscard]] float raymarchTerrainCameraLimit(const glm::vec3& pivot, const glm::vec3& camDir,
                                     float maxDist) const;

    // The terrain a camera at (x, y) must stay above, or nothing when the
    // character is under the heightfield (a cave, a tunnel) and the terrain
    // over them is a roof rather than a floor.
    [[nodiscard]] std::optional<float> cameraTerrainFloor(float x, float y,
                                                          const glm::vec3& targetPos) const;

    // Swimming
    bool swimming = false;
    bool wasSwimming = false;
    static constexpr float SWIM_SPEED_FACTOR = 0.67f;
    static constexpr float SWIM_GRAVITY = -5.0f;
    static constexpr float SWIM_BUOYANCY = 8.0f;
    static constexpr float SWIM_SINK_SPEED = -3.0f;
    // How fast vertical motion bleeds off when no swim-up or swim-down key is
    // held. Retail holds depth rather than drifting, so this only arrests the
    // momentum carried in from a jump or a dive.
    static constexpr float SWIM_VERTICAL_DAMPING = 6.0f;
    // Within this distance of the surface a body floats up to it; deeper than
    // this it holds depth. Retail lets a character idle underwater - and drown
    // doing it - so buoyancy cannot reach all the way down.
    static constexpr float SWIM_FLOAT_BAND = 1.3f;
    // How far the feet float below the surface while treading: three quarters
    // of the collision height (stock 0x75F9D0 runs the swim trace with a box
    // 0.75 h tall against the liquid surface, which is a ceiling to it). 1.52
    // for a human, 0.79 for a gnome, 1.91 for a draenei.
    [[nodiscard]] float swimFloatDepth() const {
        return game::stock_movement::swimFloatDepth(collider_.height);
    }

    // The player's collision box: CreatureModelData's CollisionWidth/Height of
    // the current display (shapeshift form included), or the mount's
    // MountHeight on top of the rider (game/stock_movement.hpp). Until the
    // model is known, a human's.
    game::stock_movement::Collider collider_{};
    // Set by the frame whose swimmer jumped out at the surface: the JUMP goes
    // out from the water, and stock sends no STOP_SWIM / STOP_ASCEND for it
    // (the jump clears both flags itself, 0x988370).
    bool surfaceHopThisFrame_ = false;

    // Movement input suppression (after teleport/portal, ignore held keys)
    float movementSuppressTimer_ = 0.0f;
    // Gravity suspension (after world entry, hold Z until ground detected)
    float gravitySuspendTimer_ = 0.0f;
    float entryFloorAnchorZ_ = 0.0f;
    float entryFloorAnchorTimer_ = 0.0f;

    // State
    bool enabled = true;
    bool sitting = false;
    /// Seated in something rather than on the ground: a chair, a bench, the
    /// barber's chair. Held apart from `sitting` because only this one means
    /// the character's height is the server's to decide.
    bool seatedInChair_ = false;
    /// The server's last placement and how long ago it was; see
    /// noteServerPlacement.
    static constexpr float kServerPlacementWindow = 2.0f;
    glm::vec3 serverPlacement_{0.0f};
    float serverPlacementAge_ = 1.0e9f;
    bool barberView_ = false;
    float savedBarberYaw_ = 0.0f;
    float savedBarberPitch_ = 0.0f;
    float savedBarberDistance_ = 10.0f;
    float savedBarberPivot_ = 0.0f;
    bool ignoreSlopeLimit_ = false;
    float maxDistanceFactor_ = 1.0f;
    bool walking_ = false; // Persistent TOGGLERUN state; held Ctrl is a temporary override.
    bool runPace = false;
    bool autoRunning = false;

    // Auto-follow target position (WoW /follow). Non-null when following.
    const glm::vec3* autoFollowTarget_ = nullptr;
    static constexpr float FOLLOW_STOP_DIST = 3.0f;   // Stop within 3 units of target
    static constexpr float FOLLOW_MAX_DIST  = 40.0f;  // Cancel if > 40 units away

    // Movement state tracking (for sending opcodes on state change)
    bool wasMovingForward = false;
    bool wasMovingBackward = false;
    bool wasStrafingLeft = false;
    bool wasStrafingRight = false;
    bool wasTurningLeft = false;
    bool wasTurningRight = false;
    bool wasJumping = false;
    /// The walk/run mode last told the server (MSG_MOVE_SET_WALK_MODE /
    /// SET_RUN_MODE, events 0x12 / 0x11 in 0x6EF860).
    bool wasWalkMode_ = false;
    bool wasFalling = false;
    /// How long the player has been off the ground and moving downward.
    ///
    /// A landing is only worth telling the server about if there was a fall.
    /// Riding over uneven ground breaks contact for a frame at a time, and each
    /// of those used to send MSG_MOVE_FALL_LAND - which every other client in
    /// range plays as a landing, so a mounted player crossing a field looked to
    /// everyone else like one landing over and over.
    float airborneSeconds_ = 0.0f;
    bool wasAscending_ = false;   // Space held while flyingActive_
    bool wasDescending_ = false;  // X held while flyingActive_
    /// True while the tile under a position has not finished streaming, which
    /// is when there is no floor to find and gravity must wait for one.
    [[nodiscard]] bool groundNotStreamedYet(float x, float y) const;

    bool rotationSuppressed_ = false;
    bool steering_ = false;
    bool moveForwardActive = false;
    bool moveBackwardActive = false;
    bool strafeLeftActive = false;
    bool strafeRightActive = false;
    bool turningLeftActive = false;
    bool turningRightActive = false;

    // Movement callback
    MovementCallback movementCallback;
    StandUpCallback standUpCallback_;
    SitDownCallback sitDownCallback_;
    AutoFollowCancelCallback autoFollowCancelCallback_;

    // Movement speeds
    bool useWoWSpeed = false;
    static constexpr float WOW_RUN_SPEED = 7.0f;     // Normal run (WotLK)
    static constexpr float WOW_WALK_SPEED = 2.5f;    // Walk
    static constexpr float WOW_BACK_SPEED = 4.5f;    // Backpedal
    static constexpr float WOW_TURN_SPEED = 180.0f;  // Keyboard turn deg/sec
    // Stock CMovement constants (game/stock_movement.hpp): gravity 0xA32F74,
    // jump speed 0xAA33DC.
    static constexpr float WOW_GRAVITY = -19.2911053f;
    static constexpr float WOW_JUMP_VELOCITY = 7.95554733f;
    static constexpr float MOUNT_GRAVITY = -18.0f;       // Snappy WoW-feel jump
    static constexpr float MOUNT_JUMP_HEIGHT = 1.0f;    // Desired jump height in meters

    // Computed jump velocity using vz = sqrt(2 * g * h)
    static inline float getMountJumpVelocity() {
        return std::sqrt(2.0f * std::abs(MOUNT_GRAVITY) * MOUNT_JUMP_HEIGHT);
    }

    /// The speeds the server last gave, or the stock defaults where it gave
    /// none (stock_movement::Speeds).
    [[nodiscard]] game::stock_movement::Speeds stockSpeeds() const {
        game::stock_movement::Speeds sp;
        auto use = [](float v, float limit, float& out) {
            if (v > 0.0f && v < limit && !std::isnan(v)) out = v;
        };
        use(walkSpeedOverride_, 100.0f, sp.walk);
        use(runSpeedOverride_, 100.0f, sp.run);
        use(runBackSpeedOverride_, 100.0f, sp.runBack);
        use(swimSpeedOverride_, 100.0f, sp.swim);
        use(swimBackSpeedOverride_, 100.0f, sp.swimBack);
        use(flightSpeedOverride_, 200.0f, sp.flight);
        use(flightBackSpeedOverride_, 200.0f, sp.flightBack);
        use(turnRateOverride_, 20.0f, sp.turnRate);
        return sp;
    }

    // Server-driven speed overrides (0 = use hardcoded default)
    float currentMoveSpeed_ = 0.0f;
    float runSpeedOverride_ = 0.0f;
    float walkSpeedOverride_ = 0.0f;
    float swimSpeedOverride_ = 0.0f;
    float swimBackSpeedOverride_ = 0.0f;
    float flightSpeedOverride_ = 0.0f;
    float flightBackSpeedOverride_ = 0.0f;
    float runBackSpeedOverride_ = 0.0f;
    float turnRateOverride_ = 0.0f;  // rad/s; 0 = WOW_TURN_SPEED default (π rad/s)
    // Server-driven root state: when true, block all horizontal movement input.
    bool movementRooted_ = false;
    bool noStrafe_ = false;
    glm::vec3 pivotOffset_{0.0f};
    bool noJump_ = false;
    // Server-driven gravity disable (levitate/hover): skip gravity accumulation.
    bool gravityDisabled_ = false;
    // Server-driven feather fall: cap downward velocity to slow-fall terminal.
    bool featherFallActive_ = false;
    // Server-driven water walk: treat water surface as ground (don't swim).
    bool waterWalkActive_ = false;
    // Player-controlled flight (CAN_FLY + FLYING): 3D movement, no gravity.
    bool flyingActive_ = false;
    bool flightAirborne_ = false;
    float flightPitchDeg_ = 0.0f;
    // Server-driven hover (HOVER flag): float at fixed height above ground.
    bool hoverActive_ = false;
    bool mounted_ = false;
    float mountHeightOffset_ = 0.0f;
    bool externalMoving_ = false;

    // Online mode: trust server position, don't prefer outdoors over WMO floors
    bool onlineMode = false;

    // Default spawn position (Goldshire Inn)
    glm::vec3 defaultPosition = glm::vec3(-9464.0f, 62.0f, 200.0f);
    float defaultYaw = 0.0f;
    float defaultPitch = -5.0f;

    // Spawn intro camera pan
    bool introActive = false;
    float introTimer = 0.0f;
    float introDuration = 0.0f;
    float introStartYaw = 0.0f;
    float introEndYaw = 0.0f;
    float introOrbitDegrees = 0.0f;
    float introStartPitch = -15.0f;
    float introEndPitch = -5.0f;
    float introStartDistance = 12.0f;
    float introEndDistance = 10.0f;

    // Idle camera: triggers intro pan after IDLE_TIMEOUT seconds of no input
    float idleTimer_ = 0.0f;
    bool idleOrbit_ = false;  // true when current intro pan is an idle orbit (loops)
    bool idleOrbitEnabled_ = false;  // stock CGCamera has no idle camera
    static constexpr float IDLE_TIMEOUT = 120.0f; // 2 minutes

    // Last known safe position (saved periodically when grounded on real geometry)
    bool hasLastSafe_ = false;
    glm::vec3 lastSafePos_ = glm::vec3(0.0f);
    float safePosSaveTimer_ = 0.0f;
    bool hasRealGround_ = false; // True only when terrain/WMO/M2 floor is detected
    static constexpr float SAFE_POS_SAVE_INTERVAL = 2.0f; // Save every 2 seconds

    // No-ground timer: after grace period, let the player fall instead of hovering
    float noGroundTimer_ = 0.0f;
    // Edge trigger for the terrain-penetration rescue's log line. It fires every
    // frame of a steep climb, and the interesting event is that it started -
    // above all if it ever starts somewhere it should not, like a crypt.
    bool terrainRescueActive_ = false;
    /// Throttle for the WOWEE_FLOOR_DEBUG trace.
    float floorDebugTimer_ = 0.0f;
    static constexpr float NO_GROUND_GRACE = 0.5f; // 500ms grace for terrain streaming

    // Continuous fall time (for auto-unstuck detection)
    float continuousFallTime_ = 0.0f;
    bool autoUnstuckFired_ = false;
    AutoUnstuckCallback autoUnstuckCallback_;
    static constexpr float AUTO_UNSTUCK_FALL_TIME = 5.0f; // 5 seconds of falling

    // Collision query cache (skip expensive checks if position barely changed)
    glm::vec3 lastCollisionCheckPos_ = glm::vec3(0.0f);
    float cachedFloorHeight_ = 0.0f;
    bool hasCachedFloor_ = false;
    static constexpr float COLLISION_CACHE_DISTANCE = 0.15f;  // Re-check every 15cm

    // Server-driven knockback state.
    // When the server sends SMSG_MOVE_KNOCK_BACK, we apply horizontal + vertical
    // impulse here and let the normal physics loop (gravity, collision) resolve it.
    bool knockbackActive_ = false;
    glm::vec2 knockbackHorizVel_ = glm::vec2(0.0f); // render-space horizontal velocity (units/s)
    // Horizontal velocity decays via WoW-like drag so the player doesn't slide enhanced.
    static constexpr float KNOCKBACK_HORIZ_DRAG = 4.5f; // exponential decay rate (1/s)

    // The stock camera's shakes (0x606970): the strongest of each direction
    // plays.
    struct StockShake {
        int type = 0, direction = 0;
        float amplitude = 0.0f, frequency = 0.0f, duration = 0.0f, phase = 0.0f, coefficient = 0.0f;
        glm::vec3 source{0.0f};
        float elapsed = 0.0f;
    };
    std::vector<StockShake> stockShakes_;
    glm::vec3 stockShakeOffset(float deltaTime);

    // Camera shake state (SMSG_CAMERA_SHAKE)
    float shakeElapsed_   = 0.0f;
    float shakeDuration_  = 0.0f;
    float shakeMagnitude_ = 0.0f;
    float shakeFrequency_ = 0.0f;
    float shakeScale_     = 1.0f;  ///< the player's Camera shake setting

    // Server-authored drunkenness (0 sober, 1 smashed).
    float intoxication_ = 0.0f;
    float intoxicationTime_ = 0.0f;
    float drunkLevel_ = 0.0f;
    bool drunkWobble_ = false;
};

} // namespace rendering
} // namespace wowee
