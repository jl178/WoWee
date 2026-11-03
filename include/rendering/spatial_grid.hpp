#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace rendering {

/// The uniform grid the M2 and WMO renderers narrow a collision query with.
///
/// Both kept their own copy of this, identical down to the hash constants and
/// the cell size, because both answer the same question: which of the tens of
/// thousands of instances in a zone could possibly overlap this box. Scanning
/// them all is what it replaces.
///
/// A miss here does not crash. It hands the caller too few candidates and the
/// player walks through a wall. Both callers used to cover for that by scanning
/// every instance whenever the grid answered nothing, which on open ground is
/// every query and cost 24 ms a frame in Hellfire Peninsula; they now trust an
/// empty answer, and what makes that safe - every box overlapping the query is
/// offered, through any sequence of files, erases and refiles - is pinned by
/// tests/test_spatial_grid.cpp.

/// 64 world units, which is a little under an ADT chunk.
inline constexpr float kSpatialCellSize = 64.0f;

struct GridCell {
    int x;
    int y;
    int z;
    bool operator==(const GridCell& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct GridCellHash {
    size_t operator()(const GridCell& c) const {
        size_t h1 = std::hash<int>()(c.x);
        size_t h2 = std::hash<int>()(c.y);
        size_t h3 = std::hash<int>()(c.z);
        return h1 ^ (h2 * 0x9e3779b9u) ^ (h3 * 0x85ebca6bu);
    }
};

/// Cell to the instance ids that touch it.
using SpatialGrid = std::unordered_map<GridCell, std::vector<uint32_t>, GridCellHash>;

/// The cell a world position falls in.
///
/// std::floor rather than a cast to int, and that is the whole trap: a cast
/// truncates towards zero, so every position between -64 and 0 would land in
/// cell 0 alongside the positions just above it, and the cells eastward of the
/// map origin would each be double width.
inline GridCell toSpatialCell(const glm::vec3& p) {
    return GridCell{
        .x = static_cast<int>(std::floor(p.x / kSpatialCellSize)),
        .y = static_cast<int>(std::floor(p.y / kSpatialCellSize)),
        .z = static_cast<int>(std::floor(p.z / kSpatialCellSize))
    };
}

/// Files an instance under every cell its world bounds reach into.
inline void insertBounds(SpatialGrid& grid, const glm::vec3& boundsMin,
                         const glm::vec3& boundsMax, uint32_t id) {
    const GridCell minCell = toSpatialCell(boundsMin);
    const GridCell maxCell = toSpatialCell(boundsMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                grid[GridCell{.x = x, .y = y, .z = z}].push_back(id);
            }
        }
    }
}

/// Takes an instance back out of every cell its bounds reached.
///
/// Called with the bounds the instance had when it was filed, not its current
/// ones: an instance that has moved is erased with its old box and filed again
/// with its new one, and passing the new box to both leaves it listed in cells
/// it has left.
///
/// A cell emptied this way is left in place rather than erased. Zones churn
/// instances constantly and the cell will be filled again.
inline void eraseBounds(SpatialGrid& grid, const glm::vec3& boundsMin,
                        const glm::vec3& boundsMax, uint32_t id) {
    const GridCell minCell = toSpatialCell(boundsMin);
    const GridCell maxCell = toSpatialCell(boundsMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                auto it = grid.find(GridCell{.x = x, .y = y, .z = z});
                if (it == grid.end()) continue;
                auto& ids = it->second;
                ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            }
        }
    }
}

/// Moves an instance from the box it was filed under to its current one.
///
/// An instance that has moved but still covers the same run of cells needs no
/// work at all, and that is the common case: units and doodads move a few
/// units per frame inside cells 64 wide. Refiling anyway is not merely wasted
/// work, it is a linear erase across every cell the instance touches, once per
/// moving instance per frame.
///
/// Answers whether the grid changed, which is what a cache of query answers
/// (GridQueryCache below) has to be told.
inline bool refileBounds(SpatialGrid& grid,
                         const glm::vec3& oldMin, const glm::vec3& oldMax,
                         const glm::vec3& newMin, const glm::vec3& newMax,
                         uint32_t id) {
    if (toSpatialCell(oldMin) == toSpatialCell(newMin) &&
        toSpatialCell(oldMax) == toSpatialCell(newMax)) {
        return false;
    }
    eraseBounds(grid, oldMin, oldMax, id);
    insertBounds(grid, newMin, newMax, id);
    return true;
}

/// The distinct ids filed in any cell the query box reaches, in the order the
/// grid walks them. An instance spanning several cells is offered once.
///
/// `seen` is the caller's scratch set, passed in because both callers hold a
/// thread_local one: these queries run on the camera's async threads.
inline void gatherIds(const SpatialGrid& grid, const glm::vec3& queryMin,
                      const glm::vec3& queryMax, std::unordered_set<uint32_t>& seen,
                      const std::function<void(uint32_t)>& onId) {
    seen.clear();
    const GridCell minCell = toSpatialCell(queryMin);
    const GridCell maxCell = toSpatialCell(queryMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                auto it = grid.find(GridCell{.x = x, .y = y, .z = z});
                if (it == grid.end()) continue;
                for (uint32_t id : it->second) {
                    if (!seen.insert(id).second) continue;
                    onId(id);
                }
            }
        }
    }
}

/// A fresh layout generation, never handed out before in this process.
///
/// One counter for every grid, so that a generation identifies one grid in one
/// state: a renderer destroyed and another built at the same address cannot
/// hand a cache the answer the first one gave.
inline uint64_t nextGridLayoutGen() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/// The last few answers a grid query gave, keyed by the cells the query box
/// reached and the grid's layout generation.
///
/// A query's answer depends on its box only through the run of cells it
/// covers, so two queries a yard apart inside one 64-yard cell get the same
/// list. The M2 grid in a city holds hundreds of ids per cell, and the
/// player's floor and wall probes asked for that same list nine times a frame,
/// each time deduplicating it through a hash set and mapping every id back to
/// an index: about 20 microseconds a query. The owner bumps the generation
/// whenever the answer could change (the grid, its id map, or anything else
/// the answer was built from), and a stale entry then simply never matches.
///
/// Per thread, as the scratch sets are: the queries run on the camera's async
/// threads as well as the main one.
template <typename T>
struct GridQueryCache {
    static constexpr size_t kEntries = 4;
    struct Entry {
        uint64_t gen = 0;   // 0 is never handed out, so an empty slot never matches
        GridCell minCell{0, 0, 0};
        GridCell maxCell{0, 0, 0};
        std::vector<T> items;
    };
    Entry entries[kEntries];
    size_t nextSlot = 0;

    [[nodiscard]] const std::vector<T>* find(uint64_t gen, const GridCell& minCell,
                                             const GridCell& maxCell) const {
        for (const auto& e : entries) {
            if (e.gen == gen && e.minCell == minCell && e.maxCell == maxCell) return &e.items;
        }
        return nullptr;
    }

    void store(uint64_t gen, const GridCell& minCell, const GridCell& maxCell,
               const std::vector<T>& items) {
        Entry& e = entries[nextSlot];
        nextSlot = (nextSlot + 1) % kEntries;
        e.gen = gen;
        e.minCell = minCell;
        e.maxCell = maxCell;
        e.items.assign(items.begin(), items.end());
    }
};


/// A rectangle of cells in a dense grid, inclusive at both ends.
struct CellRange {
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;

    [[nodiscard]] size_t count() const {
        return static_cast<size_t>(maxX - minX + 1) * static_cast<size_t>(maxY - minY + 1);
    }
};

/// Which cells of a dense grid a query box covers, or nothing when it misses.
///
/// This is the other kind of grid in the renderer: where SpatialGrid above is a
/// hash of cells over a whole zone, a WMO group carries a dense grid of its own
/// triangles, sized to its bounding box. Three queries walked it, for any
/// triangle, for floors and for walls, and each computed the cell rectangle
/// itself.
///
/// A cell missed here is a triangle never tested, so the failure is falling
/// through a floor or walking through a wall, and it happens only at whatever
/// spot the arithmetic goes wrong.
///
/// The extents are guarded rather than trusted: a group whose bounding box is
/// flat in one axis would divide by zero, and a degenerate group is a real
/// thing, a doorway or a railing modelled as a single plane.
inline std::optional<CellRange> cellRangeCovering(
        int cellsX, int cellsY, float extentX, float extentY,
        const glm::vec2& gridOrigin,
        float minX, float minY, float maxX, float maxY) {
    if (cellsX <= 0 || cellsY <= 0) return std::nullopt;

    const float invCellW = static_cast<float>(cellsX) / std::max(0.01f, extentX);
    const float invCellH = static_cast<float>(cellsY) / std::max(0.01f, extentY);

    CellRange range;
    range.minX = std::max(0, static_cast<int>((minX - gridOrigin.x) * invCellW));
    range.minY = std::max(0, static_cast<int>((minY - gridOrigin.y) * invCellH));
    range.maxX = std::min(cellsX - 1, static_cast<int>((maxX - gridOrigin.x) * invCellW));
    range.maxY = std::min(cellsY - 1, static_cast<int>((maxY - gridOrigin.y) * invCellH));

    if (range.minX > range.maxX || range.minY > range.maxY) return std::nullopt;
    return range;
}

/// The columns, row by row, of the cells a segment's band reaches: every cell
/// holding a point within `margin` of the segment a-b on each axis.
///
/// A ray across a group used to gather the whole rectangle its entry and exit
/// points span, and a diagonal ray across a 64x64 grid is 4096 cells for the
/// ~190 it crosses. The band is the same margin around the same segment, so a
/// triangle the segment meets - or comes within the margin of - is still in
/// a cell it returns; the cells it drops are only those the rectangle had for
/// being square. A row the band misses gets an empty span (min > max).
///
/// Rows outside the grid clamp into its edge rows and columns, as the
/// rectangle's arithmetic (cellRangeCovering) does, so the edge rows and
/// columns reach to infinity here too.
struct ColumnSpan {
    int minX = 1;
    int maxX = 0;
};

inline void segmentBandColumns(int cellsX, int cellsY, float extentX, float extentY,
                               const glm::vec2& gridOrigin, const glm::vec2& a,
                               const glm::vec2& b, float margin, const CellRange& range,
                               std::vector<ColumnSpan>& spans) {
    spans.assign(static_cast<size_t>(range.maxY - range.minY + 1), ColumnSpan{});
    const float invCellW = static_cast<float>(cellsX) / std::max(0.01f, extentX);
    const float invCellH = static_cast<float>(cellsY) / std::max(0.01f, extentY);
    const float cellH = 1.0f / invCellH;
    // Generous at every comparison: a cell too many is a few triangles tested
    // for nothing, a cell too few is a wall walked through.
    const float slackY = margin + 1e-3f * cellH + 1e-4f;
    const float slackX = margin + 1e-3f / invCellW + 1e-4f;
    constexpr float kInf = std::numeric_limits<float>::infinity();
    for (int cy = range.minY; cy <= range.maxY; ++cy) {
        const float y0 = cy == 0 ? -kInf : gridOrigin.y + static_cast<float>(cy) * cellH;
        const float y1 = cy == cellsY - 1 ? kInf : gridOrigin.y + static_cast<float>(cy + 1) * cellH;
        const float lo = y0 - slackY, hi = y1 + slackY;
        // The part of the segment whose y is within the row, widened.
        float t0 = 0.0f, t1 = 1.0f;
        const float dy = b.y - a.y;
        if (std::abs(dy) < 1e-12f) {
            if (a.y < lo || a.y > hi) continue;
        } else {
            float ta = (lo - a.y) / dy, tb = (hi - a.y) / dy;
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(0.0f, ta);
            t1 = std::min(1.0f, tb);
            if (t0 > t1) continue;
        }
        const float xa = a.x + (b.x - a.x) * t0;
        const float xb = a.x + (b.x - a.x) * t1;
        const float minX = std::min(xa, xb) - slackX;
        const float maxX = std::max(xa, xb) + slackX;
        ColumnSpan span;
        span.minX = std::max(range.minX, static_cast<int>(std::max(-1.0e9f, (minX - gridOrigin.x) * invCellW)));
        span.maxX = std::min(range.maxX, static_cast<int>(std::min(1.0e9f, (maxX - gridOrigin.x) * invCellW)));
        // Truncation toward zero, as the grid's own arithmetic: a point left
        // of the origin lands in column 0, and so must the span.
        if ((minX - gridOrigin.x) * invCellW < 0.0f) span.minX = range.minX;
        spans[static_cast<size_t>(cy - range.minY)] = span;
    }
}

}  // namespace rendering
}  // namespace wowee
