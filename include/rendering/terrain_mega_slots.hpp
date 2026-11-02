#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// A loaded terrain tile as the mega-slot placement sees it.
struct TerrainSlotTile {
    /// From the camera to the near edge of the tile's sphere, in the plane;
    /// 0 when the camera is inside it.
    float distance = 0.0f;
    /// Holds a mega slot, whether or not every chunk is drawn from it.
    bool hasSlot = false;
    /// Has chunks drawn from their own buffers.
    bool hasFallback = false;
    /// Its chunks can be written into a slot (the CPU mesh is at hand and
    /// matches what was uploaded).
    bool promotable = false;
};

/// What the placement does this frame: nothing, write tile `tile` into a
/// free slot, or take tile `tile` out of its slot so the slot comes free.
struct TerrainSlotMove {
    enum class Kind : uint8_t { None, Promote, Evict };
    Kind kind = Kind::None;
    size_t tile = 0;
};

/// The one move that brings the slotted tiles closer to the nearest ones.
///
/// The tile wanted is the nearest promotable tile within `needDistance`
/// that has chunks on their own buffers. With a slot free it is promoted.
/// Otherwise the farthest slotted tile is evicted, but only when it lies
/// beyond `needDistance + evictMargin` and no slot is still on its way back
/// (`releasePending`): an evicted slot is written again only after the
/// frames that may draw from it have finished, and evicting more while one
/// is in flight would empty slots nothing is waiting to use. The margin
/// keeps a tile on the edge of the ring from being swapped in and out as
/// the camera moves back and forth across it. Ties go to the lower index.
inline TerrainSlotMove chooseTerrainSlotMove(const std::vector<TerrainSlotTile>& tiles,
                                             uint32_t freeSlots, bool releasePending,
                                             float needDistance, float evictMargin) {
    TerrainSlotMove move;
    size_t want = tiles.size();
    for (size_t i = 0; i < tiles.size(); ++i) {
        const TerrainSlotTile& t = tiles[i];
        if (!t.hasFallback || !t.promotable || !(t.distance <= needDistance)) continue;
        if (want == tiles.size() || t.distance < tiles[want].distance) want = i;
    }
    if (want == tiles.size()) return move;
    if (freeSlots > 0) {
        move.kind = TerrainSlotMove::Kind::Promote;
        move.tile = want;
        return move;
    }
    if (releasePending) return move;
    size_t victim = tiles.size();
    for (size_t i = 0; i < tiles.size(); ++i) {
        const TerrainSlotTile& t = tiles[i];
        if (!t.hasSlot || !(t.distance > needDistance + evictMargin)) continue;
        if (victim == tiles.size() || t.distance > tiles[victim].distance) victim = i;
    }
    if (victim == tiles.size()) return move;
    move.kind = TerrainSlotMove::Kind::Evict;
    move.tile = victim;
    return move;
}

}  // namespace wowee::rendering
