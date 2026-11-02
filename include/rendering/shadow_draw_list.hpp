#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

/// Whether a sphere lies wholly outside the clip volume of an orthographic
/// light-space matrix, so nothing inside it can reach the shadow map.
///
/// The shadow pipelines do not clamp depth, so a triangle beyond either depth
/// plane is clipped like one off the side: x and y outside [-1, 1] and z
/// outside [0, 1] all draw nothing. A point within `radius` of `centre` lands
/// within radius times the length of a row's xyz of the centre's clip
/// coordinate on that row, which is what each test allows for, plus 1e-3 for
/// rounding (the terms of a clip coordinate are a few tens at world positions
/// of ~1e4, each off by ~1e-6). A matrix with a projective bottom row, a
/// negative or non-finite radius, answer false: never culled.
inline bool sphereOutsideOrthoClip(const glm::mat4& m, const glm::vec3& centre, float radius) {
    if (m[0][3] != 0.0f || m[1][3] != 0.0f || m[2][3] != 0.0f || m[3][3] != 1.0f) return false;
    if (!(radius >= 0.0f) || !std::isfinite(radius)) return false;
    const glm::vec4 p = m * glm::vec4(centre, 1.0f);
    const auto reach = [&](int row) {
        return radius * glm::length(glm::vec3(m[0][row], m[1][row], m[2][row])) + 1e-3f;
    };
    return std::abs(p.x) > 1.0f + reach(0) || std::abs(p.y) > 1.0f + reach(1) ||
           p.z < -reach(2) || p.z > 1.0f + reach(2);
}

/// An indexed draw: indexCount indices from firstIndex, offset by vertexOffset.
struct IndexRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    int32_t vertexOffset = 0;
};

/// Grow `last` to also cover `next` when the two are one draw: same vertex
/// offset, and `next` starts at the index where `last` ends. The indices
/// are read in the same order either way, so the triangles are the same.
inline bool tryExtendIndexRange(IndexRange& last, const IndexRange& next) {
    if (last.vertexOffset != next.vertexOffset) return false;
    if (static_cast<uint64_t>(last.firstIndex) + last.indexCount != next.firstIndex) return false;
    if (static_cast<uint64_t>(last.indexCount) + next.indexCount > UINT32_MAX) return false;
    last.indexCount += next.indexCount;
    return true;
}

/// A tile's chunks with a sphere round them all. The terrain passes test a
/// tile first and skip its chunks when the tile cannot hold one that passes.
struct TileChunkGroup {
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    std::vector<uint32_t> chunkIndices;
};

/// The chunks grouped by tile, kept up to date as chunks are appended.
///
/// Tiles take their slots in the order their first chunk appears and hold
/// their chunks in order, so grouping the first n chunks and then appending
/// the rest gives exactly what grouping all of them at once gives - the same
/// slots, the same lists, and spheres computed by the same arithmetic over
/// the same lists. A removal shifts the chunks after it, so the owner starts
/// again from nothing (reset) when one has happened.
struct TileChunkGrouping {
    std::vector<TileChunkGroup> tiles;
    std::unordered_map<uint64_t, size_t> slotOf;
    /// How many chunks are grouped so far.
    size_t grouped = 0;
    /// The owner's removal count when this was last started over.
    uint64_t removalGeneration = ~0ull;

    void reset() {
        tiles.clear();
        slotOf.clear();
        grouped = 0;
    }

    /// Group chunks [grouped, count). `keyOf(i)` names chunk i's tile and
    /// `sphereOf(i, centre, radius)` gives its bounding sphere. The slots of
    /// the tiles that gained a chunk go to `touched` (when given), once each
    /// and in slot order; their spheres are recomputed.
    template <class KeyOf, class SphereOf>
    void append(size_t count, KeyOf keyOf, SphereOf sphereOf, std::vector<uint32_t>* touched) {
        if (touched) touched->clear();
        if (count <= grouped) return;
        std::vector<uint8_t> isTouched(tiles.size(), 0);
        // A tile's chunks arrive together, so the map is only asked when the
        // tile changes from one chunk to the next.
        uint64_t lastKey = 0;
        size_t lastSlot = 0;
        bool haveLast = false;
        for (size_t ci = grouped; ci < count; ++ci) {
            const uint64_t key = keyOf(ci);
            if (!haveLast || key != lastKey) {
                auto [it, inserted] = slotOf.try_emplace(key, tiles.size());
                if (inserted) {
                    tiles.emplace_back();
                    isTouched.push_back(0);
                }
                lastKey = key;
                lastSlot = it->second;
                haveLast = true;
            }
            tiles[lastSlot].chunkIndices.push_back(static_cast<uint32_t>(ci));
            isTouched[lastSlot] = 1;
        }
        grouped = count;
        for (size_t s = 0; s < tiles.size(); ++s) {
            if (!isTouched[s]) continue;
            computeSphere(tiles[s], sphereOf);
            if (touched) touched->push_back(static_cast<uint32_t>(s));
        }
    }

    template <class SphereOf>
    static void computeSphere(TileChunkGroup& tile, SphereOf sphereOf) {
        glm::vec3 sum(0.0f);
        glm::vec3 c;
        float r;
        for (uint32_t ci : tile.chunkIndices) {
            sphereOf(ci, c, r);
            sum += c;
        }
        tile.center = sum / static_cast<float>(tile.chunkIndices.empty() ? 1 : tile.chunkIndices.size());
        float reach = 0.0f;
        for (uint32_t ci : tile.chunkIndices) {
            sphereOf(ci, c, r);
            reach = std::max(reach, glm::length(c - tile.center) + r);
        }
        tile.radius = reach;
    }
};

/// Fixed-size slots handed out lowest first, so slots freed together and
/// taken again in order come back in ascending, contiguous order.
class LowestFreeSlots {
public:
    explicit LowestFreeSlots(uint32_t count = 0) : free_(count, 1), freeCount_(count) {}

    [[nodiscard]] uint32_t capacity() const { return static_cast<uint32_t>(free_.size()); }
    [[nodiscard]] uint32_t freeCount() const { return freeCount_; }

    /// The lowest free slot, now taken; -1 when none is free.
    int64_t acquire() {
        for (; scanFrom_ < free_.size(); ++scanFrom_) {
            if (free_[scanFrom_]) {
                free_[scanFrom_] = 0;
                --freeCount_;
                return static_cast<int64_t>(scanFrom_++);
            }
        }
        return -1;
    }

    /// Give a taken slot back. Out of range or already free is ignored.
    void release(uint32_t slot) {
        if (slot >= free_.size() || free_[slot]) return;
        free_[slot] = 1;
        ++freeCount_;
        if (slot < scanFrom_) scanFrom_ = slot;
    }

    void releaseAll() {
        std::fill(free_.begin(), free_.end(), 1);
        freeCount_ = capacity();
        scanFrom_ = 0;
    }

private:
    std::vector<uint8_t> free_;
    uint32_t freeCount_ = 0;
    /// No slot below this is free.
    size_t scanFrom_ = 0;
};

}  // namespace wowee::rendering
