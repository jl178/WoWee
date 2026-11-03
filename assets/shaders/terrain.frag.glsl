#version 450

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
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    ivec4 localLightTileInfo;  // local_light_tiles.glsli
    vec4 localLightTileScale;
    uvec4 localLightTiles[288];
};

#include "rt_lighting.glsli"
#include "local_light_tiles.glsli"
#include "wet_surface.glsli"
#include "stock_fog.glsli"

layout(set = 1, binding = 0) uniform sampler2D uBaseTexture;
layout(set = 1, binding = 1) uniform sampler2D uLayer1Texture;
layout(set = 1, binding = 2) uniform sampler2D uLayer2Texture;
layout(set = 1, binding = 3) uniform sampler2D uLayer3Texture;
layout(set = 1, binding = 4) uniform sampler2D uLayer1Alpha;
layout(set = 1, binding = 5) uniform sampler2D uLayer2Alpha;
layout(set = 1, binding = 6) uniform sampler2D uLayer3Alpha;

layout(set = 1, binding = 7) uniform TerrainParams {
    int layerCount;
    int hasLayer1;
    int hasLayer2;
    int hasLayer3;
    // Each layer's MCLY flags (0x7 scroll direction, 0x38 its speed, 0x40
    // scrolls, 0x80 unlit, 0x100 has an alpha map), and bit 16 when its
    // texture is the `_s` tileset, whose alpha is the specular mask.
    ivec4 layerFlags;
    // x: the map has big alpha (MPHD 0x4), so the layers are weights rather
    // than a chain of lerps. y: some layer is unlit. z: some layer scrolls.
    ivec4 chunkMode;
};

// MCSH, the chunk's baked shadow, as light: 1 lit, 0 under the shadow the map
// was baked with (trees, buildings, the hills themselves). Same 64x64 layout
// and UV as the alpha maps; chunks without one get the opaque 1x1.
layout(set = 1, binding = 8) uniform sampler2D uBakedShadow;

// What a shadow leaves of the ground (shaders\pixel\arbfp1\terrain1-3.bls:
// light * (0.7 + 0.3 * lit)). One floor for the baked MCSH shadow and the
// shadow map alike, taken as min(baked, map) first, and it darkens the whole
// lit colour - ambient included - not only the sun's share.
const float kStockShadowFloor = 0.7;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) in vec3 VertColor;   // MCCV, doubled: 1.0 is neutral
layout(location = 5) in vec3 Specular;    // the stock per-vertex highlight

layout(location = 0) out vec4 outColor;

// One texel of the shadow map, handed in by the renderer. The map is 512,
// 1024, 2048 or 4096 a side by the quality setting; this used to be a
// constant for 4096, so at 512 the filter taps all landed inside one texel
// and the bias shrank eightfold. The fallback covers a per-frame block that
// never filled the slot in, such as the character preview's.
float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

// Nine taps on a disk 1.5 texels across, turned per pixel by interleaved
// gradient noise. The 3x3 box this replaces had the same nine taps on a grid,
// and a grid's penumbra is a staircase of the map's own texels; a rotated
// disk trades that for a fine grain the eye reads as a soft edge. Repeated in
// terrain, m2, wmo and character .frag - keep the four the same.
float shadowIgn(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// The stock point lights (Wow.exe 12340, docs/re/characters.md section 1
// "Point lights: full rule"): only M2 type-1 lights (the renderer gathers no
// other kind with the Torch and lamp light setting off), the N nearest by 3D
// distance with no range cut-off, each colour * max(N.L, 0) / (0.7 d +
// 0.03 d^2). Stock picks the N per object (a model, a WMO group, a terrain
// chunk); here per pixel, which is the same set away from the seams.
vec3 stockPointLights(vec3 pos, vec3 normal, int maxLights) {
    float bestD[4] = float[4](1e30, 1e30, 1e30, 1e30);
    int bestI[4] = int[4](-1, -1, -1, -1);
    for (int i = 0; i < min(localLightMeta.x, 64); ++i) {
        vec3 d3 = localLightPosRadius[i].xyz - pos;
        float dsq = dot(d3, d3);
        for (int k = 0; k < 4; ++k) {
            if (k >= maxLights) break;
            if (dsq < bestD[k]) {
                for (int m = 3; m > k; --m) { bestD[m] = bestD[m - 1]; bestI[m] = bestI[m - 1]; }
                bestD[k] = dsq; bestI[k] = i;
                break;
            }
        }
    }
    vec3 sum = vec3(0.0);
    for (int k = 0; k < 4; ++k) {
        if (k >= maxLights || bestI[k] < 0) break;
        vec3 toLight = localLightPosRadius[bestI[k]].xyz - pos;
        float dist = sqrt(bestD[k]);
        float ndl = max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += localLightColorIntensity[bestI[k]].rgb * localLightColorIntensity[bestI[k]].w * ndl /
               max(0.7 * dist + 0.03 * dist * dist, 0.05);
    }
    return sum;
}

float sampleShadowPCF(sampler2DShadow smap, vec3 coords) {
    // The stock filter (Wow.exe 0x8742e0, docs/re/world-lighting.md "Shadow
    // map filter"): the centre and four fixed taps, k0 k2 k4 k6 in texels,
    // equal weights. The rotated disk below is the Soft setting.
    if ((localLightMeta.z & 8) == 0) {
        float t = shadowTexel();
        float s = texture(smap, coords)
                + texture(smap, vec3(coords.xy + vec2( 0.8, -1.0) * t, coords.z))
                + texture(smap, vec3(coords.xy + vec2( 0.2, -0.6) * t, coords.z))
                + texture(smap, vec3(coords.xy + vec2(-0.6, -0.2) * t, coords.z))
                + texture(smap, vec3(coords.xy + vec2(-1.0, -0.4) * t, coords.z));
        return s * 0.2;
    }
    // A Vogel (golden-angle) disk: r = sqrt((i + 0.5) / 9), i x 137.5 degrees.
    const vec2 kDisk[9] = vec2[9](
        vec2( 0.2357,  0.0000), vec2(-0.3010,  0.2758), vec2( 0.0461, -0.5250),
        vec2( 0.3794,  0.4949), vec2(-0.6963, -0.1232), vec2( 0.6596, -0.4196),
        vec2(-0.2206,  0.8207), vec2(-0.4207, -0.8101), vec2( 0.9129,  0.3334));
    float a = 6.2831853 * shadowIgn(gl_FragCoord.xy);
    float c = cos(a), s = sin(a);
    mat2 rot = mat2(c, s, -s, c) * (1.5 * shadowTexel());
    float shadow = 0.0;
    for (int i = 0; i < 9; ++i) {
        shadow += texture(smap, vec3(coords.xy + rot * kDisk[i], coords.z));
    }
    return shadow / 9.0;
}

vec3 localLightContribution(vec3 pos, vec3 normal, vec3 albedo) {
    // Torch and lamp light off: the stock rule, the 3 nearest M2 lights.
    if ((localLightMeta.z & 4) == 0) return albedo * stockPointLights(pos, normal, 3);
    vec3 sum = vec3(0.0);
    // Only the lights that can reach this pixel's tile, in the same order
    // (local_light_tiles.glsli).
    uvec2 lightMask = localLightTileMask();
    for (int word = 0; word < 2; ++word) {
    uint bits = word == 0 ? lightMask.x : lightMask.y;
    while (bits != 0u) {
        int i = findLSB(bits) + 32 * word;
        bits &= bits - 1u;
        vec3 toLight = localLightPosRadius[i].xyz - pos;
        float radius = localLightPosRadius[i].w;
        // Rejected on the squared distance, before the square root and the
        // divide: most of the sixty-four are out of range of any one pixel,
        // and this is what each of them costs.
        float distSq = dot(toLight, toLight);
        if (radius <= 0.0 || distSq >= radius * radius) continue;
        float dist = sqrt(distSq);
        float attenuation = 1.0 - dist / radius;
        attenuation *= attenuation;
        float wrappedDiffuse = 0.22 + 0.78 * max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb *
               (localLightColorIntensity[i].w * attenuation * wrappedDiffuse);
    }
    }
    return sum;
}

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * (view * vec4(worldPos, 1.0));
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// The zone's distance fog, then the air in front of it. The distance fog is
// the far haze the sky is painted to meet, so it goes on first; the volume is
// everything between the camera and that, sunlit shafts and torch glow
// included.
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    float fogFactor = stockFogVisibility(worldPos);
    color = mix(fogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

// The stock layer scroll (MCLY 0x40), in repeats, as stock_terrain::animOffset:
// eight directions stepped by the frame time and wrapped to 0 at 64 (only
// upward), divided by the speed table (0x7831a0, 0x7d2d70, 0xaf14f8).
vec2 layerScroll(int flags) {
    if ((flags & 0x40) == 0) return vec2(0.0);
    const vec2 kDir[8] = vec2[8](vec2(-1, 0), vec2(-1, 1), vec2(0, 1), vec2(1, 1),
                                 vec2(1, 0), vec2(1, -1), vec2(0, -1), vec2(-1, -1));
    const float kSpeed[8] = float[8](64.0, 48.0, 32.0, 16.0, 8.0, 4.0, 2.0, 1.0);
    vec2 d = kDir[flags & 7];
    float t = fogParams.z;
    vec2 acc = vec2(d.x > 0.0 ? mod(t, 64.0) : d.x * t, d.y > 0.0 ? mod(t, 64.0) : d.y * t);
    float k = (1.0 / 0.24) / kSpeed[(flags >> 3) & 7] * 0.125;
    return -acc.yx * k;
}

// A layer's weight from its alpha map; 0 without one (MCLY 0x100 clear), as
// the client reads its zero row for it (0x7b74a0).
float layerAlpha(int has, int flags, sampler2D alphaTex) {
    if (has == 0 || (flags & 0x100) == 0) return 0.0;
    return texture(alphaTex, LayerUV).r;
}

// rgb the texture, a its specular mask (the `_s` alpha, else none).
vec4 layerTexel(sampler2D tex, int flags) {
    vec2 uv = TexCoord;
    if (chunkMode.z != 0) uv += layerScroll(flags);
    vec4 t = texture(tex, uv);
    t.a = (flags & 0x10000) != 0 ? t.a : 0.0;
    return t;
}

void main() {
    float fragDist = length(viewPos.xyz - FragPos);

    // The stock blend (shaders\pixel\arbfp1\terrain1.bls, picked by 0x79e5c0):
    // without big alpha a chain of lerps, each layer over the result so far;
    // with it (Northrend) independent weights, the base taking what is left.
    // Both written as four weights, so a layer whose weight is nothing is
    // never fetched.
    float a1 = layerAlpha(hasLayer1, layerFlags.y, uLayer1Alpha);
    float a2 = layerAlpha(hasLayer2, layerFlags.z, uLayer2Alpha);
    float a3 = layerAlpha(hasLayer3, layerFlags.w, uLayer3Alpha);
    vec4 w;
    if (chunkMode.x != 0) {
        w = vec4(1.0 - clamp(a1 + a2 + a3, 0.0, 1.0), a1, a2, a3);
    } else {
        w = vec4((1.0 - a1) * (1.0 - a2) * (1.0 - a3), a1 * (1.0 - a2) * (1.0 - a3),
                 a2 * (1.0 - a3), a3);
    }
    const float kSkip = 0.002;
    vec4 t0 = w.x > kSkip ? layerTexel(uBaseTexture, layerFlags.x) : vec4(0.0);
    vec4 t1 = w.y > kSkip ? layerTexel(uLayer1Texture, layerFlags.y) : vec4(0.0);
    vec4 t2 = w.z > kSkip ? layerTexel(uLayer2Texture, layerFlags.z) : vec4(0.0);
    vec4 t3 = w.w > kSkip ? layerTexel(uLayer3Texture, layerFlags.w) : vec4(0.0);
    vec4 finalColor = t0 * w.x + t1 * w.y + t2 * w.z + t3 * w.w;

    vec3 norm = normalize(Normal);

    // No bump from the texture's luminance, as stock has none on the ground.
    // Its derivatives come per 2x2 pixel quad, so on distant cracked ground
    // facing a low sun the tilted normals lit as blocky glitter.

    // Rain-wet ground (wet_surface.glsli): 0, and untouched, while it is dry
    // or the setting is off.
    vec3 geomNorm = normalize(Normal);
    float wet = wetSurfaceAmount(geomNorm);
    if (wet > 0.0) finalColor.rgb = wetSurfaceAlbedo(finalColor.rgb, wet);

    // The stock terrain light (shaders\vertex\arbvp1\terrain.bls): the
    // area's ambient plus its sun by Lambert, clamped at one per channel - it
    // travels in a vertex colour - then times the MCCV colour (or a half
    // without one) and doubled in the pixel shader. VertColor arrives already
    // doubled, 1.0 neutral, so light * VertColor * texture is the same thing.
    // The clamp is the client's: where ambient + sun passes one in some
    // channel, that channel stops and the hue leans toward the others.
    vec3 lightDir2 = normalize(-lightDir.xyz);
    float diff = max(dot(norm, lightDir2), 0.0);

    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = lightDir2;
        float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(norm, ldir)));
        vec3 biasedPos = FragPos + norm * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            // shadowParams.y is the player's shadow strength over the stock
            // 0.3; 1 at the default leaves the map's 0..1 as it is.
            shadow = mix(1.0, shadow, clamp(shadowParams.y / (1.0 - kStockShadowFloor), 0.0, 1.0));
        }
    }

    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow);
    // The baked shadow against the live one, the darker of the two (the
    // stock shader's MIN): a tree the dynamic map already shades is not
    // shaded twice, and ground beyond its reach keeps the baked shadow.
    shadow = min(shadow, texture(uBakedShadow, LayerUV).r);

    vec3 light = min(rtAmbient(rt, ambientColor.rgb) + diff * lightColor.rgb, vec3(1.0));
    vec3 lit = light * VertColor;
    vec3 result = lit * finalColor.rgb;
    if (chunkMode.y != 0) {
        // MCLY 0x80, an unlit layer (lava): its texture as it is, where the
        // others take primary * texture * 2 (terrain1.bls variants 16-31,
        // c1 = 0 for the layer, 0x7d2d70). Blended by the same weights.
        vec3 s0 = (layerFlags.x & 0x80) != 0 ? t0.rgb : t0.rgb * lit;
        vec3 s1 = (layerFlags.y & 0x80) != 0 ? t1.rgb : t1.rgb * lit;
        vec3 s2 = (layerFlags.z & 0x80) != 0 ? t2.rgb : t2.rgb * lit;
        vec3 s3 = (layerFlags.w & 0x80) != 0 ? t3.rgb : t3.rgb * lit;
        result = s0 * w.x + s1 * w.y + s2 * w.z + s3 * w.w;
    }
    result *= kStockShadowFloor + (1.0 - kStockShadowFloor) * shadow;
    // The highlight, masked by the blended `_s` alpha and the shadow, added
    // after the floor and not doubled (terrain1.bls: spec * tex.a * lit).
    result += Specular * finalColor.a * shadow;

    result += localLightContribution(FragPos, norm, finalColor.rgb);
    // The film over the ground is smoother than the ground: half the bump.
    if (wet > 0.0) {
        result += wetSurfaceSheen(normalize(mix(geomNorm, norm, 0.5)), FragPos, fragDist,
                                  wet, shadow);
    }

    result = applyFog(result, FragPos, fragDist);

    outColor = vec4(result, 1.0);
}
