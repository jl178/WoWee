#pragma once

#include "game/taxi_rules.hpp"

#include "game/world_packets.hpp"
#include "core/coordinates.hpp"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include <glm/glm.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace game {

class GameHandler;
enum class SpeedKind : uint8_t;  // game/unit_motion_glue.hpp

class MovementHandler {
public:
    using PacketHandler = std::function<void(network::Packet&)>;
    using DispatchTable = std::unordered_map<LogicalOpcode, PacketHandler>;

    explicit MovementHandler(GameHandler& owner);

    void registerOpcodes(DispatchTable& table);

    // --- Public API (delegated from GameHandler) ---

    void sendMovement(Opcode opcode);
    void setPosition(float x, float y, float z);
    void setOrientation(float orientation);
    void setMovementPitch(float radians) { movementInfo.pitch = radians; }
    void dismount();

    // Follow target (moved from GameHandler)
    void followTarget();
    void followUnit(uint64_t guid);
    void cancelFollow();
    // Per-frame movement toward the followed target's live position, called
    // from GameHandler::update() alongside the existing followRenderPos_
    // refresh. A no-op when no follow target is set. Recomputes the target
    // position fresh every call (from the live entity, not a stored point),
    // so it doesn't need the long-distance Z-interpolation guard the
    // headless client's single-shot /movement/goto needs - there's never a
    // large stale remaining distance to interpolate across.
    void updateFollowMovement(float deltaTime);

    // Area trigger detection
    void loadAreaTriggerDbc();
    void checkAreaTriggers();

    // Transport attachment
    void setTransportAttachment(uint64_t childGuid, ObjectType type, uint64_t transportGuid,
                                const glm::vec3& localOffset, bool hasLocalOrientation,
                                float localOrientation,
                                bool offsetNeedsTransportResolution = false);
    void clearTransportAttachment(uint64_t childGuid);
    void updateAttachedTransportChildren(float deltaTime);

    // ---- The active mover (stock 0x729010 / 0x72CCA0) ----
    //
    // The unit this client moves: the player, or the vehicle (or possessed
    // unit) SMSG_CLIENT_CONTROL_UPDATE hands it. movementInfo and the speeds
    // below are the active mover's; the player's own are kept aside while
    // another unit is moved, and the movement packets, force acks and
    // CMSG_SET_ACTIVE_MOVER carry the mover's guid.
    [[nodiscard]] uint64_t activeMoverGuid() const;
    [[nodiscard]] bool movingOtherUnit() const;
    /// Make `guid` (0: the player) the active mover: stops the old one,
    /// loads the new one's movement and speeds, sends CMSG_SET_ACTIVE_MOVER.
    void setActiveMover(uint64_t guid);
    /// The player is the mover again without a word to the server (a world
    /// entry, which sends its own CMSG_SET_ACTIVE_MOVER).
    void resetActiveMover() {
        if (movingOtherUnit()) loadMoverState(playerState_);
        moverGuid_ = 0;
    }
    /// The player's own movement while another unit is moved (else movementInfo).
    [[nodiscard]] const MovementInfo& playerMovementInfo() const {
        return movingOtherUnit() ? playerState_.info : movementInfo;
    }
    /// A movement packet of the active mover with `opcode`, then whatever
    /// `extra` appends (CMSG_CHANGE_SEATS_ON_CONTROLLED_VEHICLE,
    /// CMSG_DISMISS_CONTROLLED_VEHICLE, a cast's trajectory block).
    void sendMoverPacket(Opcode opcode, const std::function<void(network::Packet&)>& extra);
    /// The active mover's movement block as it would go out now, server axes.
    [[nodiscard]] MovementInfo moverWireInfo();
    /// Writes the active mover's packed guid and movement block into `packet`.
    void writeMoverMovement(network::Packet& packet);

    // Movement info accessors
    [[nodiscard]] const MovementInfo& getMovementInfo() const { return movementInfo; }
    MovementInfo& getMovementInfoMut() { return movementInfo; }

    // Speed accessors
    [[nodiscard]] float getServerRunSpeed() const { return serverRunSpeed_; }
    [[nodiscard]] float getServerWalkSpeed() const { return serverWalkSpeed_; }
    [[nodiscard]] float getServerSwimSpeed() const { return serverSwimSpeed_; }
    [[nodiscard]] float getServerSwimBackSpeed() const { return serverSwimBackSpeed_; }
    [[nodiscard]] float getServerFlightSpeed() const { return serverFlightSpeed_; }
    [[nodiscard]] float getServerFlightBackSpeed() const { return serverFlightBackSpeed_; }
    [[nodiscard]] float getServerRunBackSpeed() const { return serverRunBackSpeed_; }
    [[nodiscard]] float getServerTurnRate() const { return serverTurnRate_; }
    [[nodiscard]] float getServerPitchRate() const { return serverPitchRate_; }

    // CREATE_OBJECT/LIVING carries the authoritative speed table on login.
    // Apply it to the same cache used by camera movement and later force-speed
    // packets so reconnecting while mounted retains the server-provided speed.
    void applyServerMovementSpeeds(float walk, float run, float runBack,
                                   float swim, float swimBack, float flight,
                                   float flightBack, float turn, float pitch);

    // Movement flag queries
    [[nodiscard]] bool isPlayerRooted() const { return movementInfo.isPlayerRooted(); }
    [[nodiscard]] bool isGravityDisabled() const { return movementInfo.isGravityDisabled(); }
    [[nodiscard]] bool isFeatherFalling() const { return movementInfo.isFeatherFalling(); }
    [[nodiscard]] bool isWaterWalking() const { return movementInfo.isWaterWalking(); }
    [[nodiscard]] bool isPlayerFlying() const { return movementInfo.isPlayerFlying(); }
    [[nodiscard]] bool isHovering() const { return movementInfo.isHovering(); }
    [[nodiscard]] bool isSwimming() const { return movementInfo.isSwimming(); }

    // Taxi / Flight Paths
    [[nodiscard]] bool isTaxiWindowOpen() const { return taxiWindowOpen_; }
    void closeTaxi();
    void activateTaxi(uint32_t destNodeId);
    [[nodiscard]] bool isOnTaxiFlight() const { return onTaxiFlight_; }

    /// True while a locally-initiated dismount is waiting on the server to
    /// agree. The player's mount field keeps its old value for a few frames
    /// after the request, and taking that at face value re-mounts them.
    [[nodiscard]] bool isDismountPending() const { return dismountGraceRemaining_ > 0.0f; }
    void clearDismountPending() { dismountGraceRemaining_ = 0.0f; }
    [[nodiscard]] bool isTaxiMountActive() const { return taxiMountActive_; }
    [[nodiscard]] bool isTaxiActivationPending() const { return taxiActivatePending_; }
    void forceClearTaxiAndMovementState();
    [[nodiscard]] const std::string& getTaxiDestName() const { return taxiDestName_; }
    [[nodiscard]] const ShowTaxiNodesData& getTaxiData() const { return currentTaxiData_; }
    [[nodiscard]] uint32_t getTaxiCurrentNode() const { return currentTaxiData_.nearestNode; }

    struct TaxiNode {
        uint32_t id = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
        std::string name;
        uint32_t mountDisplayIdAlliance = 0;
        uint32_t mountDisplayIdHorde = 0;
    };
    struct TaxiPathEdge {
        uint32_t pathId = 0;
        uint32_t fromNode = 0, toNode = 0;
        uint32_t cost = 0;
    };
    struct TaxiPathNode {
        uint32_t id = 0;
        uint32_t pathId = 0;
        uint32_t nodeIndex = 0;
        uint32_t mapId = 0;
        float x = 0, y = 0, z = 0;
    };

    [[nodiscard]] const std::unordered_map<uint32_t, TaxiNode>& getTaxiNodes() const { return taxiNodes_; }

    /// Drops what was read out of the taxi DBCs, so the next ask reloads them.
    ///
    /// Called when the active expansion changes. GameHandler::resetDbcCaches
    /// cleared copies of its own for this, and getTaxiNodes forwards here - so
    /// switching expansion left the previous one's flight points in place and
    /// nothing said so.
    void resetTaxiDbcCache() {
        taxiNodes_.clear();
        taxiPathEdges_.clear();
        taxiPathNodes_.clear();
        taxiDbcLoaded_ = false;
    }
    // WotLK 3.3.5a TaxiNodes.dbc has 384 entries; the known-taxi bitmask
    // is 12 × uint32 = 384 bits. Node IDs outside this range are invalid.
    static constexpr uint32_t kMaxTaxiNodeId = 384;
    [[nodiscard]] bool isKnownTaxiNode(uint32_t nodeId) const {
        if (nodeId == 0 || nodeId > kMaxTaxiNodeId) return false;
        uint32_t idx = nodeId - 1;
        return (knownTaxiMask_[idx / 32] & (1u << (idx % 32))) != 0;
    }
    [[nodiscard]] uint32_t getTaxiCostTo(uint32_t destNodeId) const;
    /// True when the taxi cost map has a route from the current node to dest.
    [[nodiscard]] bool hasTaxiRouteTo(uint32_t destNodeId) const;
    /// Node-id hop chain current → dest (inclusive); empty if unreachable.
    [[nodiscard]] std::vector<uint32_t> getTaxiRouteTo(uint32_t destNodeId) const;
    [[nodiscard]] bool taxiNpcHasRoutes(uint64_t guid) const {
        auto it = taxiNpcHasRoutes_.find(guid);
        return it != taxiNpcHasRoutes_.end() && it->second;
    }

    void updateClientTaxi(float deltaTime);
    /// A spline of the server's own moves the player (a charge, a leap, a
    /// scripted MovePoint, an exit off a seat - not a taxi flight, which has
    /// its own path above, nor a vehicle's seat). The stock client runs it as
    /// any unit's (no active-player branch in 0x73C8E0): the controls do
    /// nothing (0x5FA060), no movement packet goes out (0x6F09F0 asks
    /// 0x4F5240 for a running spline) and the player is where the spline has
    /// them.
    [[nodiscard]] bool playerOnServerSpline() const;
    /// Each frame after the units moved: the player's position follows that
    /// spline, and at its end (0x6EB0B0 -> 0x6EAE70 -> 0x71F210) the player,
    /// on the end point with forward and the pending stop cleared
    /// (0x98BD10), sends CMSG_MOVE_SPLINE_DONE with the spline's id.
    void updateServerSpline();
    /// The flight is over (the server cleared UNIT_FLAG_TAXI_FLIGHT after its
    /// spline ran): back on the ground, MSG_MOVE_STOP.
    void endTaxiFlight();
    /// The UI error text for an SMSG_ACTIVATETAXIREPLY result.
    [[nodiscard]] std::string taxiReplyErrorText(uint32_t result) const;
    uint32_t nextMovementTimestampMs();
    void sanitizeMovementForTaxi();

    // Heartbeat / movement timing (for GameHandler::update())
    float& timeSinceLastMoveHeartbeatRef() { return timeSinceLastMoveHeartbeat_; }
    /// Whether the stock client would be sending heartbeats now: a direction,
    /// falling, ascending or descending (stock_movement::kMovingMask, 0x6F09F0).
    /// Turning in place, pitching and treading water send none.
    [[nodiscard]] bool inStockHeartbeatMotion() const;
    /// Whether a facing change is worth a MSG_MOVE_SET_FACING: at least 0.1 rad
    /// from the facing the last movement packet of any kind carried (0x71AE80).
    /// canonicalYaw is in the same space as movementInfo.orientation.
    [[nodiscard]] bool facingChangeWorthSending(float canonicalYaw) const;
    /// MSG_MOVE_SET_PITCH due: swimming or flying, 0.1 rad from the pitch the
    /// last movement packet carried (stock_movement::pitchChangeWorthSending).
    [[nodiscard]] bool pitchChangeWorthSending(float pitchRad) const;
    /// A facing the server put the player in (a spline's final facing): the
    /// reference later facing changes are measured from, as if a packet had
    /// carried it - stock applies it without an event 0x13 (0x98C480), so no
    /// SET_FACING follows.
    void noteServerFacing(float canonicalYaw) {
        lastPacketOrientation_ = canonicalYaw;
        hasLastPacketOrientation_ = true;
    }
    /// A fall that began by walking off an edge, reported as stock reports it
    /// (event 9): FALLING with v0 0, the leg's direction and speed, a
    /// HEARTBEAT sent with the fall time counted from the edge.
    void beginWalkOffFall(uint32_t msSinceEdge);
    /// A reported fall that lands without a FALL_LAND (within 1/36 yd of its
    /// apex): FALLING cleared and the pending keys applied, nothing sent.
    void landWithoutPacket();
    [[nodiscard]] float getMoveHeartbeatInterval() const { return moveHeartbeatInterval_; }
    [[nodiscard]] bool isServerMovementAllowed() const { return serverMovementAllowed_; }
    /// Whether the player is in the air on a flying mount, as the movement
    /// code worked it out - which sets and clears FLYING, the flag the server
    /// reads flight from. Only while flight is allowed: a FLYING the server
    /// set by itself is left alone.
    void setFlightAirborne(bool airborne);
    /// Where the last movement packet that went out put the player, in
    /// canonical coordinates - which is where the server has them - and false
    /// before any has.
    [[nodiscard]] bool lastSentPosition(glm::vec3& out) const {
        if (!hasSentMovement_) return false;
        out = lastSentPos_;
        return true;
    }
    void setServerMovementAllowed(bool v) { serverMovementAllowed_ = v; }
    /// A far teleport (SMSG_NEW_WORLD to another map) is loading. Until the
    /// world loader has put the character at the destination, the position and
    /// facing the renderer reports are the old map's, so they are not taken
    /// and nothing is sent: the stock client sends MSG_MOVE_WORLDPORT_ACK only
    /// once the new map is loaded and no movement before it (0x403B70).
    void endWorldPortHold();
    [[nodiscard]] bool isWorldPortHeld() const;
    uint32_t& monsterMovePacketsThisTickRef() { return monsterMovePacketsThisTick_; }
    uint32_t& monsterMovePacketsDroppedThisTickRef() { return monsterMovePacketsDroppedThisTick_; }

    // Movement clock / fall state setters (formerly accessed via friend)
    void resetMovementClock() { movementClockStart_ = std::chrono::steady_clock::now(); lastMovementTimestampMs_ = 0; }
    void setFalling(bool falling) { isFalling_ = falling; }
    void setFallStartMs(uint32_t ms) { fallStartMs_ = ms; }

    // Taxi state references for GameHandler update/processing
    bool& onTaxiFlightRef() { return onTaxiFlight_; }
    bool& taxiMountActiveRef() { return taxiMountActive_; }
    bool& taxiActivatePendingRef() { return taxiActivatePending_; }
    float& taxiActivateTimerRef() { return taxiActivateTimer_; }
    bool& taxiClientActiveRef() { return taxiClientActive_; }
    float& taxiLandingCooldownRef() { return taxiLandingCooldown_; }
    float& taxiStartGraceRef() { return taxiStartGrace_; }
    bool& taxiRecoverPendingRef() { return taxiRecoverPending_; }
    uint32_t& taxiRecoverMapIdRef() { return taxiRecoverMapId_; }
    glm::vec3& taxiRecoverPosRef() { return taxiRecoverPos_; }
    std::unordered_map<uint64_t, bool>& taxiNpcHasRoutesRef() { return taxiNpcHasRoutes_; }
    uint32_t* knownTaxiMaskPtr() { return knownTaxiMask_; }
    bool& taxiMaskInitializedRef() { return taxiMaskInitialized_; }
    uint64_t& taxiNpcGuidRef() { return taxiNpcGuid_; }
    [[nodiscard]] uint64_t getTaxiNpcGuid() const { return taxiNpcGuid_; }

    // Other-player movement timing (for cleanup on despawn etc.)
    std::unordered_map<uint64_t, uint32_t>& otherPlayerMoveTimeMsRef() { return otherPlayerMoveTimeMs_; }
    std::unordered_map<uint64_t, float>& otherPlayerSmoothedIntervalMsRef() { return otherPlayerSmoothedIntervalMs_; }

    // Methods also called from GameHandler's registerOpcodeHandlers
    void handleCompressedMoves(network::Packet& packet);
    void handleForceMoveFlagChange(network::Packet& packet, const char* name, Opcode ackOpcode, uint32_t flag, bool set);
    void handleMoveSetCollisionHeight(network::Packet& packet);

private:
    /// Another unit's speed that a spline speed opcode sets.
    static SpeedKind remoteSpeedKind(float MovementHandler::* member);

    // --- Packet handlers ---
    void handleMonsterMove(network::Packet& packet);
    void handleMonsterMoveTransport(network::Packet& packet);
    void handleOtherPlayerMovement(network::Packet& packet);
    void handleMoveSetSpeed(network::Packet& packet);
    void handleForceRunSpeedChange(network::Packet& packet);
    network::Packet buildForceAck(Opcode ackOpcode, uint32_t counter);
    void handleForceSpeedChange(network::Packet& packet, const char* name, Opcode ackOpcode, float* speedStorage);
    void handleForceMoveRootState(network::Packet& packet, bool rooted);
    void handleMoveKnockBack(network::Packet& packet);
    void handleTeleportAck(network::Packet& packet);
    void handleNewWorld(network::Packet& packet);
    void handleShowTaxiNodes(network::Packet& packet);
    void handleClientControlUpdate(network::Packet& packet);
    void handleActivateTaxiReply(network::Packet& packet);
    void loadTaxiDbc();

    // --- Private helpers ---
    void buildTaxiCostMap();
    bool restoreWorldTransferFallbackIfNearOrigin(const char* context);

    GameHandler& owner_;

    // --- Movement state ---
    // Reference to GameHandler's movementInfo to avoid desync
    MovementInfo& movementInfo;
    std::chrono::steady_clock::time_point movementClockStart_ = std::chrono::steady_clock::now();
    uint32_t lastMovementTimestampMs_ = 0;
    bool serverMovementAllowed_ = true;
    uint32_t monsterMovePacketsThisTick_ = 0;
    uint32_t monsterMovePacketsDroppedThisTick_ = 0;

    // Fall/jump tracking
    bool isFalling_ = false;
    uint32_t fallStartMs_ = 0;

    // Heartbeat timing
    int heartbeatLogCount_ = 0;  // periodic position audit counter
    uint32_t lastAreaTriggerId_ = 0;
    uint32_t postTransferReturnAreaTriggerId_ = 0;
    bool postTransferReturnAreaTriggerSawNear_ = false;
    bool pendingAreaTriggerDestinationValid_ = false;
    uint32_t pendingAreaTriggerDestinationMapId_ = 0;
    glm::vec3 pendingAreaTriggerDestinationServerPos_{0.0f};
    float pendingAreaTriggerDestinationServerO_ = 0.0f;
    bool worldTransferFallbackValid_ = false;
    uint32_t worldTransferFallbackMapId_ = 0;
    uint32_t worldTransferFallbackTriggerId_ = 0;
    glm::vec3 worldTransferFallbackCanonicalPos_{0.0f};
    float worldTransferFallbackCanonicalO_ = 0.0f;
    float timeSinceLastMoveHeartbeat_ = 0.0f;
    // Seconds left in which a stale non-zero mount field is not to be believed.
    // Bounded so a dismount the server refuses recovers on its own rather than
    // leaving the player permanently unable to look mounted.
    float dismountGraceRemaining_ = 0.0f;
    float moveHeartbeatInterval_ = 0.5f;
    uint32_t lastHeartbeatSendTimeMs_ = 0;
    float lastHeartbeatX_ = 0.0f;
    float lastHeartbeatY_ = 0.0f;
    float lastHeartbeatZ_ = 0.0f;
    uint32_t lastHeartbeatFlags_ = 0;
    uint64_t lastHeartbeatTransportGuid_ = 0;
    uint32_t lastNonHeartbeatMoveSendTimeMs_ = 0;
    glm::vec3 lastSentPos_{0.0f};
    bool hasSentMovement_ = false;
    /// Why movement is being held back from the server, said once per stretch
    /// of it. See sendMovement.
    const char* movementHeldReason_ = nullptr;
    /// Set by a far teleport, cleared by endWorldPortHold (or after a minute,
    /// so a load that never finishes cannot hold movement for good).
    bool worldPortHold_ = false;
    std::chrono::steady_clock::time_point worldPortHoldSince_{};
    uint32_t lastFacingSendTimeMs_ = 0;
    float lastFacingSentOrientation_ = 0.0f;
    /// The facing the last movement packet of any kind carried, the reference
    /// the stock client measures a facing change from (CGUnit +0xA50, written
    /// by 0x71F0C0). Unset until the first packet.
    float lastPacketOrientation_ = 0.0f;
    float lastPacketPitch_ = 0.0f;
    /// The fall time a walk-off fall's HEARTBEAT backdates to its edge.
    uint32_t fallBackdateMs_ = 0;
    /// The jump block's direction and speed (+0x70/+0x74, +0x8C) for the
    /// current flags, as a jump or a fall from an edge starts them.
    void setFallLeg();
    /// An acknowledgement carrying the mover's movement went out: the
    /// heartbeat restarts and its facing and pitch are the reference, as for
    /// any movement packet (0x71F0C0).
    void noteMovementPacketSent();
    bool hasLastPacketOrientation_ = false;

    // The active mover (0: the player) and the player's own movement and
    // speeds while another unit is the mover.
    uint64_t moverGuid_ = 0;
    struct MoverState {
        MovementInfo info;
        float walk = 2.5f, run = 7.0f, runBack = 4.5f, swim = 4.722f, swimBack = 2.5f;
        float flight = 7.0f, flightBack = 4.5f, turn = core::coords::PI, pitch = core::coords::PI;
        bool allowed = true;
    };
    MoverState playerState_;
    MoverState captureMoverState() const;
    void loadMoverState(const MoverState& s);
    /// Where a force-move packet about `guid` applies: the active mover's
    /// state (movementInfo and the speeds), the player's kept-aside one, or
    /// nothing (another unit).
    enum class ForceTarget { Mover, KeptPlayer, None };
    ForceTarget forceTargetOf(uint64_t guid) const;
    network::Packet buildForceAckFor(Opcode ackOpcode, uint32_t counter, uint64_t guid,
                                     const MovementInfo& info);

    // Speed state
    float serverRunSpeed_ = 7.0f;
    float serverWalkSpeed_ = 2.5f;
    float serverRunBackSpeed_ = 4.5f;
    float serverSwimSpeed_ = 4.722f;
    float serverSwimBackSpeed_ = 2.5f;
    float serverFlightSpeed_ = 7.0f;
    float serverFlightBackSpeed_ = 4.5f;
    float serverTurnRate_ = core::coords::PI;
    float serverPitchRate_ = core::coords::PI;

    // Other-player movement smoothing
    std::unordered_map<uint64_t, uint32_t> otherPlayerMoveTimeMs_;
    std::unordered_map<uint64_t, float>    otherPlayerSmoothedIntervalMs_;

    // --- Taxi / Flight Path state ---
    std::unordered_map<uint64_t, bool> taxiNpcHasRoutes_;
    std::unordered_map<uint32_t, TaxiNode> taxiNodes_;
    std::vector<TaxiPathEdge> taxiPathEdges_;
    std::unordered_map<uint32_t, std::vector<TaxiPathNode>> taxiPathNodes_;
    bool taxiDbcLoaded_ = false;
    bool taxiWindowOpen_ = false;
    ShowTaxiNodesData currentTaxiData_;
    uint64_t taxiNpcGuid_ = 0;
    /// Triggers already reported as a near miss, so the log says it once.
    std::set<uint32_t> nearMissLogged_;
    bool onTaxiFlight_ = false;
    std::string taxiDestName_;
    bool taxiMountActive_ = false;
    bool taxiActivatePending_ = false;
    float taxiActivateTimer_ = 0.0f;
    // How long to wait for SMSG_ACTIVATETAXIREPLY before giving up - see
    // updateClientTaxi()'s comment on why a dropped reply needs this now.
    static constexpr float kTaxiActivateReplyTimeoutSeconds = 8.0f;
    bool taxiClientActive_ = false;
    // updateServerSpline: a server spline has the player (and its id).
    bool serverSplineActive_ = false;
    uint32_t serverSplineId_ = 0;
    float taxiLandingCooldown_ = 0.0f;
    float taxiStartGrace_ = 0.0f;
    bool taxiRecoverPending_ = false;
    uint32_t taxiRecoverMapId_ = 0;
    glm::vec3 taxiRecoverPos_{0.0f};
    uint32_t knownTaxiMask_[12] = {};
    bool taxiMaskInitialized_ = false;
    std::unordered_map<uint32_t, uint32_t> taxiCostMap_;
    std::vector<taxi::Edge> taxiRouteEdges_;
    bool taxiSplineSeen_ = false;
    // Shortest-route predecessor per node from buildTaxiCostMap; getTaxiRouteTo walks it.
    std::unordered_map<uint32_t, uint32_t> taxiPrevMap_;

    bool followMoveMoving_ = false;  // whether MSG_MOVE_START_FORWARD has been sent for the current follow
};

} // namespace game
} // namespace wowee
