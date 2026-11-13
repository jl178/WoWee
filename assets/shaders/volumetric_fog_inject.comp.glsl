#version 450

// Lights the air in one cell of the fog volume. See volumetric_fog.hpp.
//
// Each invocation writes, for its cell, the light the air there scatters
// toward the camera per yard (rgb) and how much of the light passing through
// it the air takes out per yard (a). It samples one point inside the cell,
// moved each frame, and blends that with the same stretch of air last frame -
// so a shadow edge between two cells resolves over a few frames instead of
// stepping from one cell to the next.

// A group is 4x4 cells across and four slices deep - every other slice of
// eight (groupCell): the slices carried from last frame (vol.dims.w) are
// then whole groups, which return before the light gather. The volume's
// depth is a multiple of eight at every quality (volumetric_fog.cpp).
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

// The cell an invocation lights (fogInjectCell in volumetric_fog_math.hpp
// mirrors this, tested). Group z = 2b + p takes slices 8b + p,
// 8b + p + 2, 8b + p + 4 and 8b + p + 6: every slice once, and the four in a
// group of one parity.
ivec3 groupCell(uvec3 group, uvec3 local) {
    uint z = 8u * (group.z >> 1) + (group.z & 1u) + 2u * local.z;
    return ivec3(group.xy * 4u + local.xy, z);
}

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;
    vec4 playerWake;
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    // The rest of the block as far as the detail map of the sun's shadows
    // (shadow_filter.glsli), for crisp canopy beams near the camera.
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    ivec4 localLightTileInfo;
    vec4 localLightTileScale;
    uvec4 localLightTiles[288];
    mat4 nearLightSpaceMatrix;
    vec4 nearShadowParams;
    uvec4 nearShadowMask[32];
    mat4 detailLightSpaceMatrix;
    vec4 detailShadowParams;  // x = on, y = one texel in uv, z = 1 / depth range (yd), w = yards a texel
};

// The detail map of the sun's shadows (renderer binding 6): 0.07 yd a texel
// over 72 yd round the player, the leaves cut out.
layout(set = 0, binding = 6) uniform sampler2DShadow uDetailShadowMap;

layout(set = 1, binding = 0) uniform VolumeParams {
    vec4 rayOrigin;      // xyz = camera position
    vec4 rayCorner[4];   // rays one yard deep through uv (0,0), (1,0), (0,1), (1,1)
    mat4 prevViewProj;
    vec4 medium;         // x = density, y = layer base, z = 1 / layer height, w = floor
    vec4 lighting;       // x = sun scatter, y = local light scatter, z = ambient scatter,
                         // w = the forward lobe's share (0 under a sky with no sun disc)
    vec4 drift;          // xyz = how far the banks have blown, in noise cells (mist_wind.hpp), w = noise amount
    vec4 jitter;         // xyz = sample point inside the cell, w = history weight
    ivec4 dims;          // xyz = size, w = 0 every cell lit, 1 or 2 only odd or even slices
    vec4 skyField;       // the sky height field: corner xy, height of depth 0, w = 1 when drawn
    vec4 dawn;           // xyz toward the sun's sprite, w = the far air's share of it
    vec4 shafts;         // the reference client's canopy shafts: x = strength (0 = stock), y = how far it
                         // is night (the moonlight's share), z = probe radius in yards,
                         // w = 1 / the yards over which they fade out with distance
    vec4 wind;           // xyz = how far the wisps have blown and churned, in their noise cells,
                         // w = their share of the density (mist_wind.hpp)
    vec4 lines;          // the shafts as lines (enhanced_shafts::Lines): x = the share of the air's
                         // sky light kept by night, y, z = how far a beam is held to a thin one by
                         // night and by day, w = the gain the beams left take, as far as held
} vol;

layout(set = 1, binding = 1) uniform sampler2D uShadowDepth;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image3D uCellsOut;
layout(set = 1, binding = 3) uniform sampler3D uHistory;
// Whether the sun reached each cell, for the canopy shafts: this frame's
// (written only while they are on) and last frame's, read round a cell.
layout(set = 1, binding = 6, r32f) uniform writeonly image3D uVisibilityOut;
layout(set = 1, binding = 7) uniform sampler3D uVisibilityHistory;
// The sky height field round the camera (SkyHeightField): the tops of what
// stands, seen from above, and the ground alone - for the dawn rays.
layout(set = 1, binding = 8) uniform sampler2D uSkyTops;
layout(set = 1, binding = 9) uniform sampler2D uSkyGround;
const float kSkySpan = 256.0;   // sky_field::kSpan
const float kSkyDepth = 400.0;  // sky_field::kDepth

// How far along the view slice coordinate s lies: 0 at the near edge, the
// slice count at the far one, spaced so each slice is a fixed fraction deeper
// than the one before.
float sliceDepth(float s) {
    return volumetricParams.y * exp(s / (volumetricParams.w * volumetricParams.z));
}

vec3 rayAt(vec2 uv) {
    vec3 top = mix(vol.rayCorner[0].xyz, vol.rayCorner[1].xyz, uv.x);
    vec3 bottom = mix(vol.rayCorner[2].xyz, vol.rayCorner[3].xyz, uv.x);
    return mix(top, bottom, uv.y);
}

// On the lattice's own period (mist_wind::kPeriodCells), so the offsets the
// wind accumulates wrap without a seam however long a session runs.
float hash13(vec3 p) {
    p = mod(p, 256.0);
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

float valueNoise3D(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i);
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

// Extinction per yard. Densest at the layer's base - the ground the camera
// stands over - thinning with height to a floor that never quite clears, and
// broken into banks forty yards or so across that drift on the zone's wind
// (mist_wind.hpp: dust across Westfall, mist creeping through Duskwood), and
// finer wisps eight yards across on more of it, turning over on their own,
// so the air moves through the beams rather than sliding past as one sheet.
// Both slow - a few yards a second at most - and soft-edged, so the history
// carries them without a shimmer. Thinned to nothing over the back two
// thirds of the volume's depth: this is the near air, where shafts can be
// seen, and the zone's distance fog is the haze past it.
float airDensity(vec3 p, float depth) {
    float above = max(p.z - vol.medium.y, 0.0);
    float layer = mix(vol.medium.w, 1.0, exp(-above * vol.medium.z));
    vec3 q = p / 40.0 + vol.drift.xyz;
    float n = valueNoise3D(q) * 0.65 + valueNoise3D(q * 2.0 + 11.0) * 0.35;
    float banks = mix(1.0, 2.0 * n, vol.drift.w);
    float wisp = valueNoise3D(p * (5.0 / 40.0) + vol.wind.xyz);
    banks *= 1.0 + vol.wind.w * (2.0 * wisp - 1.0);
    float far = sliceDepth(volumetricParams.w);
    float near = 1.0 - smoothstep(0.35 * far, far, depth);
    return vol.medium.x * layer * banks * near;
}

// Whether the sun reaches this point, off the same shadow map the surfaces
// use. One tap and no filter: the jitter and the history do the softening.
// Fades out toward the edge of the map rather than ending on a wall of lit
// air where its coverage stops.
float worldSunVisibility(vec3 p) {
    if (shadowParams.x < 0.5) return 1.0;
    vec4 ls = lightSpaceMatrix * vec4(p, 1.0);
    vec3 proj = ls.xyz / ls.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z < 0.0 || proj.z > 1.0) {
        return 1.0;
    }
    float stored = textureLod(uShadowDepth, uv, 0.0).r;
    float lit = (proj.z - 0.0005 <= stored) ? 1.0 : 0.0;
    vec2 edge = abs(proj.xy);
    return mix(lit, 1.0, smoothstep(0.8, 1.0, max(edge.x, edge.y)));
}

// With the canopy shafts on, near the player the detail map instead: one
// bilinear compare at 0.07 yd a texel, where the world map's 0.26 yd texel,
// taken once a cell and smeared by the jitter and the history, left a beam
// a soft glow with no edge. Eased into the world map over the detail map's
// outer tenth, as the surfaces do (shadow_filter.glsli detailShadowAt).
float sunVisibility(vec3 p) {
    if (shadowParams.x < 0.5) return 1.0;
    if (vol.shafts.x <= 0.0 || detailShadowParams.x < 0.5) return worldSunVisibility(p);
    vec4 ls = detailLightSpaceMatrix * vec4(p, 1.0);
    vec2 uv = ls.xy * 0.5 + 0.5;
    float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    if (edge <= 0.01 || ls.z < 0.0 || ls.z > 1.0) return worldSunVisibility(p);
    float w = smoothstep(0.01, 0.09, edge);
    float z = ls.z - 2.0 * detailShadowParams.w * detailShadowParams.z;
    float detail = texture(uDetailShadowMap, vec3(uv, z));
    return w >= 1.0 ? detail : mix(worldSunVisibility(p), detail, w);
}

// How high what stands over p's ground reaches (the sky height field's top
// over its ground there, yards); 0 with no field. Two taps.
float standingOver(vec3 p) {
    if (vol.skyField.w < 0.5) return 0.0;
    vec2 uv = (p.xy - vol.skyField.xy) / kSkySpan;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
    return kSkyDepth * (textureLod(uSkyGround, uv, 0.0).r - textureLod(uSkyTops, uv, 0.0).r);
}

// Where a world point lies in last frame's volume, as texture coordinates;
// false if outside it.
bool lastFrameCell(vec3 p, out vec3 prev) {
    vec4 prevClip = vol.prevViewProj * vec4(p, 1.0);
    if (prevClip.w <= 1e-3) return false;
    prev = vec3(prevClip.xy / prevClip.w * 0.5 + 0.5,
                log(max(prevClip.w, volumetricParams.y) / volumetricParams.y) * volumetricParams.z);
    return all(greaterThanEqual(prev, vec3(0.0))) && all(lessThanEqual(prev, vec3(1.0)));
}

// How much of the air round p the sun does not reach: four taps on a level
// ring vol.shafts.z yards across, turned a little every frame (the jitter) so
// the history fills it in, read from last frame's sun visibility per cell
// rather than from the shadow map: four scattered taps of the map per lit
// cell cost the Iris Xe 4 to 20 ms in a forest, the small volume next to
// nothing. Level, not square to the light: near the ground a ring square to
// a high sun dips below it and reads the ground's own shadow, which lifted
// the air over every open square. 0 in the open, where nothing near casts a
// shadow, toward 1 deep in the shade: what makes a lit cell part of a beam is
// shaded air beside it - a gap in a canopy, not a field in the sun. Taps that
// fall outside last frame's view are left out.
vec2 shadeAround(vec3 p) {
    if (shadowParams.x < 0.5 || vol.jitter.w <= 0.0) return vec2(0.0);
    float turn = 6.2831853 * vol.jitter.x;
    vec2 cs = vec2(cos(turn), sin(turn)) * vol.shafts.z;
    vec2 ct = vec2(-cs.y, cs.x) * 3.0 * 0.7071 + cs * 3.0 * 0.7071;   // the outer ring, turned 45 degrees
    vec3 ring[8] = vec3[8](vec3(cs, 0.0), vec3(-cs, 0.0), vec3(-cs.y, cs.x, 0.0), vec3(cs.y, -cs.x, 0.0),
                           vec3(ct, 0.0), vec3(-ct, 0.0), vec3(-ct.y, ct.x, 0.0), vec3(ct.y, -ct.x, 0.0));
    vec2 shaded = vec2(0.0);
    vec2 taps = vec2(0.0);
    for (int i = 0; i < 8; ++i) {
        vec3 prev;
        if (!lastFrameCell(p + ring[i], prev)) continue;
        float s = 1.0 - textureLod(uVisibilityHistory, prev, 0.0).r;
        if (i < 4) { shaded.x += s; taps.x += 1.0; } else { shaded.y += s; taps.y += 1.0; }
    }
    return vec2(taps.x > 0.0 ? shaded.x / taps.x : 0.0, taps.y > 0.0 ? shaded.y / taps.y : 0.0);
}

// Whether the sun's own light reaches p, toward the sprite rather than along
// the light the shadow map is cast from: at dawn that light still stands
// about 15 degrees up, easing onto a sun three or four degrees up, and the
// air near the ground under the trees is all in its shadow. Marched over the
// sky height field (12 steps of 6 yards toward the sun): blocked where the
// top of what stands there, or the ground, is over the ray. -1 where the
// field does not cover p.
float dawnSunVisibility(vec3 p) {
    if (vol.skyField.w < 0.5) return -1.0;
    vec3 dir = normalize(vol.dawn.xyz);
    for (int i = 1; i <= 12; ++i) {
        vec3 q = p + dir * (float(i) * 6.0);
        vec2 uv = (q.xy - vol.skyField.xy) / kSkySpan;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return i == 1 ? -1.0 : 1.0;
        float d = min(textureLod(uSkyTops, uv, 0.0).r, textureLod(uSkyGround, uv, 0.0).r);
        if (q.z < vol.skyField.z - kSkyDepth * d - 0.5) return 0.0;
    }
    return 1.0;
}

// The local lights that reach any cell of this workgroup, in their original
// order. A group is a block of 4x4 cells over seven slices, a small chunk of
// the frustum, and a torch's reach is a dozen yards: of the sixty-odd lights a
// city gathers, a block sees a handful. Testing every light against every
// cell was most of the pass. A light is dropped only where it is certain to
// miss every point in the block, and the cells walk what is left in the same
// order - the set bits of a 64-bit mask, lowest first - so the sum each cell
// adds up is the one it added before, term for term.
shared uint sLightMask[2];
const float kLocalLightReach = 150.0;

void gatherGroupLights() {
    uint lane = gl_LocalInvocationIndex;   // 64 invocations, one light each
    uint count = uint(clamp(localLightMeta.x, 0, 64));
    if (lane < 2u) sLightMask[lane] = 0u;

    // The block's sample points lie inside the cells it covers, whose corners
    // span these uv and depth ranges. Rays one yard deep through a rectangle
    // of uv lie on one plane, so the block is a frustum slab: the convex hull
    // of its eight corners, which a sphere around those corners contains.
    vec3 lo = vec3(groupCell(gl_WorkGroupID, uvec3(0u)));
    vec3 hi = vec3(groupCell(gl_WorkGroupID, uvec3(3u))) + 1.0;
    vec2 uv0 = lo.xy / vec2(vol.dims.xy);
    vec2 uv1 = hi.xy / vec2(vol.dims.xy);
    float d0 = sliceDepth(lo.z);
    float d1 = sliceDepth(hi.z);
    vec3 corners[8];
    vec3 centre = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
        vec2 uv = vec2((i & 1) != 0 ? uv1.x : uv0.x, (i & 2) != 0 ? uv1.y : uv0.y);
        corners[i] = vol.rayOrigin.xyz + rayAt(uv) * ((i & 4) != 0 ? d1 : d0);
        centre += corners[i];
    }
    centre *= 0.125;
    float reach = 0.0;
    for (int i = 0; i < 8; ++i) reach = max(reach, distance(corners[i], centre));
    barrier();
    // Torch and lamp light only near the camera: past kLocalLightReach a
    // block is a slab tens of yards deep whose sphere takes in every light
    // in a town, and walking them all was most of the pass once the volume
    // reached 800 yards (0.3 ms became 6.6 ms in Goldshire).
    if (d0 > kLocalLightReach) count = 0u;
    if (lane < count) {
        float radius = localLightPosRadius[lane].w;
        // A margin for the rounding in placing the corners and the cells,
        // which only ever lets an extra light through.
        float limit = radius + reach + 0.05 + 0.001 * (radius + reach);
        if (radius > 0.0 && distance(localLightPosRadius[lane].xyz, centre) < limit)
            atomicOr(sLightMask[lane >> 5], 1u << (lane & 31u));
    }
    barrier();
}

// Henyey-Greenstein, scaled so scattering evenly in every direction is 1.
// How much of its sky light the canopy's shaded air loses by day, with the
// canopy shafts on: the dark air a beam is seen against.
const float kCanopyAirCut = 0.5;
const float kEnhancedSunPullback = 0.85;

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
}

void main() {
    ivec3 cell = groupCell(gl_WorkGroupID, gl_LocalInvocationID);

    // Half the slices this frame (vol.dims.w): the others keep last frame's
    // air where it was, reprojected to this frame's cell, as the blend below
    // would have kept most of it. A cell new to the view is lit regardless.
    // A group's slices are of one parity, so the whole group takes this
    // branch or none of it, and only the groups that light anything gather.
    bool carried = vol.dims.w != 0 && ((cell.z + vol.dims.w) & 1) != 0;
    if (!carried) {
        // Every invocation of the group takes part, those past the edge of
        // the volume too: the gather has barriers in it.
        gatherGroupLights();
    }
    if (any(greaterThanEqual(cell, vol.dims.xyz))) return;

    if (carried) {
        vec3 centre = vec3(cell) + 0.5;
        float cdepth = sliceDepth(centre.z);
        vec3 cp = vol.rayOrigin.xyz + rayAt(centre.xy / vec2(vol.dims.xy)) * cdepth;
        vec4 prevClip = vol.prevViewProj * vec4(cp, 1.0);
        if (prevClip.w > 1e-3) {
            vec3 prev = vec3(prevClip.xy / prevClip.w * 0.5 + 0.5,
                             log(max(prevClip.w, volumetricParams.y) / volumetricParams.y) *
                                 volumetricParams.z);
            if (all(greaterThanEqual(prev, vec3(0.0))) && all(lessThanEqual(prev, vec3(1.0)))) {
                imageStore(uCellsOut, cell, textureLod(uHistory, prev, 0.0));
                if (vol.shafts.x > 0.0)
                    imageStore(uVisibilityOut, cell, textureLod(uVisibilityHistory, prev, 0.0));
                return;
            }
        }
    }

    vec3 at = vec3(cell) + vol.jitter.xyz;
    vec2 uv = at.xy / vec2(vol.dims.xy);
    float depth = sliceDepth(at.z);
    vec3 p = vol.rayOrigin.xyz + rayAt(uv) * depth;

    float density = airDensity(p, depth);

    // The light reaching this air. Where only the sky lights it, it takes the
    // zone's fog colour, at half: shade under trees is darker than the open
    // haze, and a shaft reads against the shade beside it. Sunlight comes on
    // top, and mostly forward, as it does off mist and dust: looking at the
    // sun the air takes nineteen times the light it would scattering evenly,
    // ten degrees off it twelve, thirty degrees off two and a half, and from
    // forty five degrees on about one or less. That is where shafts are seen
    // outdoors - toward a sun low enough to look at, through whatever stands
    // in front of it.
    //
    // Only while the sun is up. At night the light's direction points up
    // from under the ground; the shadow map flips it to build its projection,
    // but flipped here the forward lobe would glow in the night sky at a
    // mirrored sun that is not there. It fades out as the sun sets instead.
    vec3 travel = normalize(lightDir.xyz);
    float sunUp = smoothstep(0.0, 0.08, -travel.z);
    vec3 toCamera = normalize(vol.rayOrigin.xyz - p);
    // Under enhanced daylight (localLightMeta.z bit 128) the light comes
    // from the sun's disc itself (enhanced_palette::sunLightDirection), so the
    // forward lobe sits on it: tighter and with less even scattering, the
    // bright air round the disc falls off as quickly as round the reference client's sun
    // (docs/re/enhanced-lighting.md 2e). Elsewhere the stock light stays
    // beside the sun and keeps the wider lobe.
    bool enhancedSun = (localLightMeta.z & 128) != 0;
    // Dawn rays (vol.dawn.w, under the enhanced light toward a sun under
    // about 15 degrees): past 40 yards the air takes its sunlight from the
    // sun's own direction (dawnSunVisibility), and its forward lobe sits on
    // the sprite - so rays break over a tree line or a rise toward the low
    // sun, as the reference's do ("rays pop over a ridge at dawn"), while
    // the surfaces and the near air keep the light they had.
    float dawnFar = vol.dawn.w * smoothstep(40.0, 60.0, depth);
    vec3 lobeTravel = dawnFar > 0.0 ? normalize(mix(travel, -normalize(vol.dawn.xyz), dawnFar)) : travel;
    float cosSun = dot(lobeTravel, toCamera);
    float phase = enhancedSun ? mix(1.0, phaseHG(cosSun, 0.9), 0.5)
                             : mix(1.0, phaseHG(cosSun, 0.75), 0.65);
    // And within about 20 degrees of the disc the lobe gives way to even
    // scattering, softly (from 25 degrees to 5): the air right round
    // the reference client's sun is no brighter than its sky (reference_02's ring falls to
    // half within 4 degrees), where the lobe laid a wide near-white disc.
    //
    // At every height of the sun: a sun just over the horizon looks along
    // the whole depth of the lit air, and with the pull-back eased off there
    // (and the lobe on the sprite) Westfall's 6:18 sun took 7.6% of the view
    // to white (docs/re/enhanced-lighting.md 5c).
    const float pullback = kEnhancedSunPullback;
    if (enhancedSun) phase = mix(phase, 1.0, pullback * smoothstep(0.906, 0.996, cosSun));
    // Under a sky with no sun disc (Outland's: a sky model takes the whole
    // sky, LightingParams::sunDisc) there is nothing to scatter toward: the
    // lobe is left out and the even share kept, so the air is no brighter
    // elsewhere and there is no glow round a sun that is not drawn.
    float evenShare = enhancedSun ? 0.5 : 0.35;
    phase = mix(evenShare, phase, vol.lighting.w);
    // By night under the shafts, most of that sky light is taken out: the
    // moonlit air read as a soft blue glow down Goldshire's road, the
    // moonbeams lost in it (Jered, 046a047). Dark air, and the beams on it.
    float skyAir = vol.shafts.x > 0.0 ? mix(1.0, vol.lines.x, vol.shafts.y) : 1.0;
    vec3 light = fogColor.rgb * (vol.lighting.z * skyAir);
    // One tap of the map, shared by the mist's sunlight and the shafts below.
    float lit = (sunUp > 0.0 || vol.shafts.x > 0.0) ? sunVisibility(p) : 0.0;
    if (dawnFar > 0.0) {
        float dawnLit = dawnSunVisibility(p);
        if (dawnLit >= 0.0) lit = mix(lit, dawnLit, dawnFar);
    }
    if (vol.shafts.x > 0.0) imageStore(uVisibilityOut, cell, vec4(lit));
    if (sunUp > 0.0) {
        // Under the enhanced light the mist's own sunlight (not the canopy
        // shafts below) fades in from the camera: 30% at 4 yards, full by
        // 40. The near air's sunlit veil over everything in front of the
        // camera eased the evening and forest views and the crowns' chroma
        // (docs/re/enhanced-lighting.md 5c, the near air).
        float nearSun = enhancedSun ? mix(0.3, 1.0, smoothstep(4.0, 40.0, depth)) : 1.0;
        light += lightColor.rgb * (lit * phase * sunUp * vol.lighting.x * nearSun);
    }

    // The reference client's canopy shafts (vol.shafts.x > 0 only under the enhanced light,
    // outdoors; docs/re/enhanced-lighting.md 5c). Its froxel fog reads the
    // sun's cascades per cell as this one does, but under a forest the beams
    // stand out bright against dark air from well off the sun. Here a lit
    // cell with shade beside it - a gap in the leaves - scatters far more,
    // with a lobe wide enough to show from forty degrees off the light, and
    // the shaded air beside it keeps its ambient. In the open there is no
    // shade near a lit cell and nothing changes. By night the same from the
    // moonlight: the light the shadow map is cast from, in the night's own
    // colour.
    if (vol.shafts.x > 0.0) {
        vec3 down = travel.z > 0.0 ? -travel : travel;
        float reach = 1.0 - clamp(depth * vol.shafts.w - 1.0, 0.0, 1.0);
        float day = sunUp * (1.0 - vol.shafts.y);
        float weight = day + vol.shafts.y;
        if (reach > 0.0 && weight > 0.0) {
            // How much of the air round this cell, on rings one and three
            // probe radii out, the light does not reach: high under a closed
            // canopy, low in the open.
            vec2 rings = shadeAround(p);
            // The canopy's own air: shaded, with shade all round it. Its
            // sky light (the fog colour) is cut by day, so a beam reads
            // against dark air rather than a lifted veil (the reference
            // stills' dark blue-green air either side of a shaft). Not to
            // black: the leaves keep their colour, and the air its depth.
            //
            // Only beside a beam: some of the inner ring lit. Deep in the
            // shade, with no beam near, the air keeps its sky light - cutting
            // it everywhere under a canopy took Elwynn's evening and forest
            // views 8% and 7% darker.
            float nearBeam = 1.0 - smoothstep(0.75, 1.0, rings.x);
            //
            // And all through the shade under tall trees (crowns topping
            // 55 to 70 yards and more, the sky height field): Ashenvale's and
            // Teldrassil's forests are closed overhead nearly everywhere,
            // and with their air at full sky light a beam in a clearing
            // stood against a teal haze as bright as itself - the
            // reference's Ashenvale is dark blue air either side of one.
            // Elwynn's oaks stand under that and keep their sky light.
            float shadeAll = (1.0 - lit) * smoothstep(0.5, 0.95, rings.y) * reach * day;
            float tallTrees = (shadeAll > 0.0 && nearBeam < 1.0) ? smoothstep(55.0, 70.0, standingOver(p)) : 0.0;
            float under = shadeAll * max(nearBeam, tallTrees);
            light -= fogColor.rgb * (vol.lighting.z * kCanopyAirCut * under);
            if (lit > 0.0) {
                // A lit cell in a gap: most of both rings in shade. A whole
                // beam qualifies, not only its rim - the outer ring reaches
                // past a gap several yards across - while a cell at the edge
                // of one broad shadow (a house, a lone tree) sees part of
                // one ring shaded and stays as it was.
                // The outer ring deep in shade: a gap in a closed canopy. The
                // sunlit air among the crowns is dappled - its outer ring half
                // lit - and passing it as a gap laid a pale veil over the
                // crowns looking up at noon.
                float gap = smoothstep(0.55, 0.9, rings.y);
                // And a gap wide enough to show as a shaft: some of the inner
                // ring lit. A hole of a yard or two among the leaves is under
                // a cell across and only blurs into the veil.
                gap *= 1.0 - smoothstep(0.7, 1.0, rings.x);
                // A line rather than a slab: half or more of the inner ring
                // shaded - a beam a few yards across, not the lit air of a
                // road or a clearing under a gap as wide, which laid a broad
                // glow. Held to that by night (vol.lines.y), and by day as
                // far as vol.lines.z.
                float lineHold = mix(vol.lines.z, vol.lines.y, vol.shafts.y);
                gap *= mix(1.0, smoothstep(0.25, 0.6, rings.x), lineHold);
                // The beams left take the gain (vol.lines.w) as far as
                // they are held: fewer, and each brighter.
                gap *= mix(1.0, vol.lines.w, lineHold);
                // Under the crowns, not in them: the sunlit air among the
                // leaves high in a canopy is dappled all through, and at
                // beam strength it laid a pale veil over the crowns looking up
                // at noon. Full to 10 yards over the ground, gone by 24 -
                // Elwynn's oaks - or, under taller trees, full to where the
                // crowns round the gap begin and gone at their tops (the sky
                // height field, the higher of two points 11 yards either
                // side, the crowns' foot taken at two fifths of it):
                // Ashenvale's and Teldrassil's reach 80 yards, and their
                // beams stand the height of the clearing rather than ending
                // in mid-air 24 yards up.
                float crownTop = 24.0;
                if (gap > 0.0) {
                    crownTop = max(crownTop, max(standingOver(p + vec3(8.0, 8.0, 0.0)),
                                                 standingOver(p - vec3(8.0, 8.0, 0.0))));
                }
                gap *= 1.0 - smoothstep(max(10.0, 0.4 * crownTop), crownTop, p.z - vol.medium.y);
                // And a few yards out: a lit hole in the crown over the camera
                // is no shaft from here, only a pale cast over the view.
                gap *= smoothstep(3.0, 10.0, depth);
                float cosL = dot(down, toCamera);
                // Forward, but visible from the side too: a shaft seen across
                // is still a shaft.
                float lobe = mix(1.0, phaseHG(cosL, 0.5), 0.7);
                // Held down round the disc, where the sky and its glow are
                // already the brightest thing on screen.
                // Only at the disc itself: looking into a sun low through the
                // trees is where the beams are, and the pull-back over the
                // twenty degrees round it had taken every beam at dawn.
                lobe = mix(lobe, 0.3, pullback * smoothstep(0.985, 0.998, cosL));
                lobe = mix(1.0, lobe, vol.lighting.w);
                // A beam is the light's hue thinned toward white, warm by
                // day; the moonbeams keep more of the night's blue.
                float thin = mix(0.35, 0.6, 1.0 - vol.shafts.y);
                vec3 beam = mix(lightColor.rgb, vec3(dot(lightColor.rgb, vec3(0.2126, 0.7152, 0.0722))), thin);
                light += beam * (vol.shafts.x * lit * gap * reach * lobe * weight);
            }
        }
    }

    // Torches, braziers and lava: the same set, falloff and intensity the
    // surfaces are lit by, so the halo is where the pool of light is.
    // A carried cell lit afresh (new to the view) had no gather: it walks
    // every light, which the radius test below cuts to the same terms.
    vec3 local = vec3(0.0);
    uint count = depth > kLocalLightReach ? 0u : uint(clamp(localLightMeta.x, 0, 64));
    uvec2 lightMask = carried
        ? uvec2(count >= 32u ? 0xFFFFFFFFu : ((1u << count) - 1u),
                count >= 64u ? 0xFFFFFFFFu : (count > 32u ? ((1u << (count - 32u)) - 1u) : 0u))
        : uvec2(sLightMask[0], sLightMask[1]);
    for (uint word = 0u; word < 2u; ++word) {
        uint bits = lightMask[word];
        while (bits != 0u) {
            uint i = word * 32u + uint(findLSB(bits));
            bits &= bits - 1u;
            vec3 toLight = localLightPosRadius[i].xyz - p;
            float radius = localLightPosRadius[i].w;
            float distSq = dot(toLight, toLight);
            if (radius <= 0.0 || distSq >= radius * radius) continue;
            float attenuation = 1.0 - sqrt(distSq) / radius;
            local += localLightColorIntensity[i].rgb *
                     (localLightColorIntensity[i].w * attenuation * attenuation);
        }
    }
    light += local * vol.lighting.y;

    vec4 result = vec4(light * density, density);

    // The same air last frame, found by projecting this point into last
    // frame's view. Anything that falls outside it is new to the view and
    // keeps this frame's sample alone.
    if (vol.jitter.w > 0.0) {
        vec4 prevClip = vol.prevViewProj * vec4(p, 1.0);
        if (prevClip.w > 1e-3) {
            vec3 prev = vec3(prevClip.xy / prevClip.w * 0.5 + 0.5,
                             log(max(prevClip.w, volumetricParams.y) / volumetricParams.y) *
                                 volumetricParams.z);
            if (all(greaterThanEqual(prev, vec3(0.0))) && all(lessThanEqual(prev, vec3(1.0)))) {
                result = mix(result, textureLod(uHistory, prev, 0.0), vol.jitter.w);
            }
        }
    }

    imageStore(uCellsOut, cell, result);
}
