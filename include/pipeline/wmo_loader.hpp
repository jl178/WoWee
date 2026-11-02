#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace wowee {
namespace pipeline {

/**
 * WMO (World Model Object) Format
 *
 * WMO files contain buildings, dungeons, and large structures.
 * Structure:
 * - Root WMO file: Contains groups, materials, doodad sets
 * - Group WMO files: Individual rooms/sections (_XXX.wmo)
 *
 * Reference: https://wowdev.wiki/WMO
 */

// WMO Material
struct WMOMaterial {
    uint32_t flags;
    uint32_t shader;
    uint32_t blendMode;
    uint32_t texture1;          // Diffuse texture index
    uint32_t color1;
    uint32_t texture2;          // Environment/detail texture
    uint32_t color2;
    uint32_t texture3;
    uint32_t color3;
    float runtime[4];           // Runtime data
};

// WMO Group Info
struct WMOGroupInfo {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    int32_t nameOffset;         // Group name in MOGN chunk
};

// WMO Light
struct WMOLight {
    uint32_t type;              // 0=omni, 1=spot, 2=directional, 3=ambient
    uint8_t useAttenuation;
    uint8_t pad[3];
    glm::vec4 color;
    glm::vec3 position;
    float intensity;
    float attenuationStart;
    float attenuationEnd;
    float unknown[4];
};

// WMO Doodad Set (collection of M2 models placed in WMO)
struct WMODoodadSet {
    char name[20];
    uint32_t startIndex;        // First doodad in MODD
    uint32_t count;             // Number of doodads
    uint32_t padding;
};

// WMO Doodad Instance
struct WMODoodad {
    uint32_t nameIndex;         // Index into MODN (doodad names)
    glm::vec3 position;
    glm::quat rotation;         // Quaternion rotation
    float scale;
    glm::vec4 color;           // BGRA color
};

// WMO Fog
struct WMOFog {
    uint32_t flags;
    glm::vec3 position;
    float smallRadius;
    float largeRadius;
    float endDist;
    float startFactor;
    glm::vec4 color1;          // End fog color
    float endDist2;
    float startFactor2;
    glm::vec4 color2;          // Start fog color (blend with color1)
};

// WMO Portal
struct WMOPortal {
    uint16_t startVertex;
    uint16_t vertexCount;
    uint16_t planeIndex;
    uint16_t padding;
    /// MOPT's plane, n.p + d = 0 (the 16 bytes after the counts). The root
    /// loader replaces a NaN plane with (0, 0, 1) (0x7d7470); the portal walk
    /// tests the camera against it with the MOPR side (0x7ac060).
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float distance = 0.0f;
};

// WMO Portal Plane
struct WMOPortalPlane {
    glm::vec3 normal;
    float distance;
};

// WMO Portal Reference (MOPR chunk) - links portals to groups
struct WMOPortalRef {
    uint16_t portalIndex;   // Index into portals array
    uint16_t groupIndex;    // Group on other side of portal
    int16_t side;           // Which side of portal plane (-1 or 1)
    uint16_t padding;
};

// WMO Liquid (MLIQ chunk data)
struct WMOLiquid {
    uint32_t xVerts = 0;        // Vertices in X direction
    uint32_t yVerts = 0;        // Vertices in Y direction
    uint32_t xTiles = 0;        // Tiles in X (= xVerts - 1)
    uint32_t yTiles = 0;        // Tiles in Y (= yVerts - 1)
    glm::vec3 basePosition;     // Corner position in model space
    /// MLIQ's own field: the MOMT material the client binds for the surface
    /// (0x7d7c30 keeps it at +0x130). Not a liquid type.
    uint16_t materialId = 0;
    /// The group's LiquidType id as the client resolves it at load
    /// (0x7d82e0 / 0x7d7310 / 0x7c8d80, stock_wmo::groupLiquidType): 13 WMO
    /// Water, 14 WMO Ocean, 19 WMO Magma, 20 WMO Slime or a DBC id; 0 none.
    uint32_t liquidTypeId = 0;
    /// The owning group's MOGP flags, for the interior-water rule at draw.
    uint32_t groupFlags = 0;
    std::vector<float> heights; // Height per vertex (xVerts * yVerts)
    std::vector<uint8_t> flags; // Flags per tile (xTiles * yTiles)

    [[nodiscard]] bool hasLiquid() const { return xVerts > 0 && yVerts > 0; }
};

// WMO Group Vertex
struct WMOVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoord;
    glm::vec4 color;           // Vertex color
    /// The second MOTV and MOCV (MOGP flags 0x2000000 / 0x1000000, 31
    /// groups of Icecrown in the data). Only MapObjUComposite reads them:
    /// UV set 1 for the second texture, MOCV2 alpha for the layer blend.
    glm::vec2 texCoord2{0.0f};
    float color2Alpha = 0.0f;
};

// WMO Batch (render batch)
struct WMOBatch {
    uint32_t startIndex;   // First index (this is uint32 in file format)
    uint16_t indexCount;   // Number of indices
    uint16_t startVertex;
    uint16_t lastVertex;
    uint8_t flags;
    uint8_t materialId;
};

// WMO Group (individual room/section)
struct WMOGroup {
    uint32_t flags;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;
    uint16_t portalStart;
    uint16_t portalCount;
    uint16_t batchCountA;
    uint16_t batchCountB;
    uint32_t fogIndices[4];     // Fog references
    uint32_t liquidType;
    uint32_t groupId;
    /// MOGP's own group id - global across all WMOs, the key WMOAreaTable
    /// and DungeonMapChunk use. Not the index above.
    uint32_t wmoGroupId = 0;

    // Geometry
    std::vector<WMOVertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<WMOBatch> batches;
    std::vector<uint8_t> triFlags;  // Per-triangle MOPY flags (0x04 = detail/no-collide)

    // Portals
    std::vector<WMOPortal> portals;
    std::vector<glm::vec3> portalVertices;

    /// MODR: the MODD doodads this group holds (0x7bf740 creates them per
    /// group, lit by the group's flags).
    std::vector<uint16_t> doodadRefs;

    // BSP tree (for collision - optional)
    std::vector<uint8_t> bspNodes;

    // Liquid data (MLIQ chunk)
    WMOLiquid liquid;

    std::string name;
    std::string description;
};

// Complete WMO Model
struct WMOModel {
    // Runtime source path when known. The binary format does not store its root
    // filename, but a few rendering classifications need the owning WMO family.
    std::string sourcePath;

    // Root WMO data (from MOHD chunk)
    uint32_t version;
    uint32_t wmoId = 0;   // MOHD wmoID: the WMOAreaTable key
    uint32_t nTextures;  // Added - was missing, caused offset issues
    uint32_t nGroups;
    uint32_t nPortals;
    uint32_t nLights;
    uint32_t nDoodadNames;
    uint32_t nDoodadDefs;
    uint32_t nDoodadSets;

    glm::vec3 ambientColor;     // MOHD ambient color (used for interior group lighting)
    /// MOHD flags: 0x1 do not attenuate vertices by portal distance, 0x2 skip
    /// the ambient when fixing vertex colours ("unified render path"), 0x4
    /// liquid type from the DBC, 0x8 do not fix vertex colours, only alpha.
    uint16_t mohdFlags = 0;
    /// MOSB: the sky model the client puts in sky slot 0 while the camera is
    /// in a group with flag 0x40000 (0x7ac060, 0x79a870). Empty: none, and
    /// the client clears every group's 0x40000 (0x7d7470).
    std::string skyboxName;
    glm::vec3 boundingBoxMin;
    glm::vec3 boundingBoxMax;

    // Materials and textures
    std::vector<WMOMaterial> materials;
    std::vector<std::string> textures;
    std::unordered_map<uint32_t, uint32_t> textureOffsetToIndex;  // MOTX offset -> texture array index

    // Groups (rooms/sections)
    std::vector<WMOGroupInfo> groupInfo;
    std::vector<WMOGroup> groups;

    // Portals (visibility culling)
    std::vector<WMOPortal> portals;
    std::vector<WMOPortalPlane> portalPlanes;
    std::vector<glm::vec3> portalVertices;
    std::vector<WMOPortalRef> portalRefs;  // MOPR chunk - portal-to-group links

    // Lights
    std::vector<WMOLight> lights;

    // Doodads (M2 models placed in WMO)
    // Keyed by byte offset into MODN chunk (nameIndex in MODD references these offsets)
    std::unordered_map<uint32_t, std::string> doodadNames;
    std::vector<WMODoodad> doodads;
    std::vector<WMODoodadSet> doodadSets;

    // Fog
    std::vector<WMOFog> fogs;

    // Group names
    std::vector<std::string> groupNames;
    std::vector<uint8_t> groupNameRaw;  // Raw MOGN chunk for offset-based name lookup

    [[nodiscard]] bool isValid() const {
        return nGroups > 0 && !groups.empty();
    }
};

class WMOLoader {
public:
    /**
     * Load root WMO file
     *
     * @param wmoData Raw WMO file bytes
     * @return Parsed WMO model (without group geometry)
     */
    static WMOModel load(const std::vector<uint8_t>& wmoData);

    /**
     * Load WMO group file
     *
     * @param groupData Raw WMO group file bytes
     * @param model Model to populate with group data
     * @param groupIndex Group index to load
     * @return True if successful
     */
    static bool loadGroup(const std::vector<uint8_t>& groupData,
                         WMOModel& model,
                         uint32_t groupIndex);
};

} // namespace pipeline
} // namespace wowee
