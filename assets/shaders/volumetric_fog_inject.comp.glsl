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
};

layout(set = 1, binding = 0) uniform VolumeParams {
    vec4 rayOrigin;      // xyz = camera position
    vec4 rayCorner[4];   // rays one yard deep through uv (0,0), (1,0), (0,1), (1,1)
    mat4 prevViewProj;
    vec4 medium;         // x = density, y = layer base, z = 1 / layer height, w = floor
    vec4 lighting;       // x = sun scatter, y = local light scatter, z = ambient scatter
    vec4 drift;          // xyz = how far the mist has blown, w = noise amount
    vec4 jitter;         // xyz = sample point inside the cell, w = history weight
    ivec4 dims;          // xyz = size, w = 0 every cell lit, 1 or 2 only odd or even slices
} vol;

layout(set = 1, binding = 1) uniform sampler2D uShadowDepth;
layout(set = 1, binding = 2, rgba16f) uniform writeonly image3D uCellsOut;
layout(set = 1, binding = 3) uniform sampler3D uHistory;

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

float hash13(vec3 p) {
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
// broken into drifting banks forty yards or so across. Thinned to nothing
// over the back two thirds of the volume's depth: this is the near air, where
// shafts can be seen, and the zone's distance fog is the haze past it.
float airDensity(vec3 p, float depth) {
    float above = max(p.z - vol.medium.y, 0.0);
    float layer = mix(vol.medium.w, 1.0, exp(-above * vol.medium.z));
    vec3 q = (p + vol.drift.xyz) / 40.0;
    float n = valueNoise3D(q) * 0.65 + valueNoise3D(q * 2.6 + 11.7) * 0.35;
    float banks = mix(1.0, 2.0 * n, vol.drift.w);
    float far = sliceDepth(volumetricParams.w);
    float near = 1.0 - smoothstep(0.35 * far, far, depth);
    return vol.medium.x * layer * banks * near;
}

// Whether the sun reaches this point, off the same shadow map the surfaces
// use. One tap and no filter: the jitter and the history do the softening.
// Fades out toward the edge of the map rather than ending on a wall of lit
// air where its coverage stops.
float sunVisibility(vec3 p) {
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

// The local lights that reach any cell of this workgroup, in their original
// order. A group is a block of 4x4 cells over seven slices, a small chunk of
// the frustum, and a torch's reach is a dozen yards: of the sixty-odd lights a
// city gathers, a block sees a handful. Testing every light against every
// cell was most of the pass. A light is dropped only where it is certain to
// miss every point in the block, and the cells walk what is left in the same
// order - the set bits of a 64-bit mask, lowest first - so the sum each cell
// adds up is the one it added before, term for term.
shared uint sLightMask[2];

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
    float phase = mix(1.0, phaseHG(dot(travel, toCamera), 0.75), 0.65);
    vec3 light = fogColor.rgb * vol.lighting.z;
    if (sunUp > 0.0) {
        light += lightColor.rgb * (sunVisibility(p) * phase * sunUp * vol.lighting.x);
    }

    // Torches, braziers and lava: the same set, falloff and intensity the
    // surfaces are lit by, so the halo is where the pool of light is.
    // A carried cell lit afresh (new to the view) had no gather: it walks
    // every light, which the radius test below cuts to the same terms.
    vec3 local = vec3(0.0);
    uint count = uint(clamp(localLightMeta.x, 0, 64));
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
