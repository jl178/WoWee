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
    mat4 nearLightSpaceMatrix;  // shadow_filter.glsli
    vec4 nearShadowParams;
    uvec4 nearShadowMask[32];
};

#include "rt_lighting.glsli"
#include "local_light_tiles.glsli"
#include "wet_surface.glsli"
#include "stock_fog.glsli"

#ifdef STREAM_FADE
// terrain_fade.frag.spv, bound only for a chunk that has just streamed in
// ("Fade in new scenery"): how much of it is still hidden, x of the vec4
// after terrain.vert's model matrix and highlight (TerrainPush::streamFade).
// The stock build has no discard at all, so it keeps its early depth test.
layout(push_constant) uniform FadePush {
    layout(offset = 80) vec4 streamFade;
} fadePush;
#include "stream_fade.glsli"
#endif

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

#include "shadow_filter.glsli"
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) in vec3 VertColor;   // MCCV, doubled: 1.0 is neutral
layout(location = 5) in vec3 Specular;    // the stock per-vertex highlight

layout(location = 0) out vec4 outColor;

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
    // Lamp light fades with the daylight around it: added at full strength
    // under a noon sun it painted a pool no eye would see there. Full at
    // night and indoors-dark, about an eighth in full day (Jered, 2026-11-09).
    float lampDaylight = dot(ambientColor.rgb + lightColor.rgb, vec3(0.299, 0.587, 0.114));
    sum *= mix(1.0, 0.12, smoothstep(0.35, 0.9, lampDaylight));
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

#ifdef CLIFF_PROJECTION
// Unstretched cliffs (E16, terrain_cliff.frag; not stock). The stock mapping
// is straight down (TexCoord: world -y, -x at eight repeats a chunk), so on a
// slope the texture is pulled out along the fall line - twice its length at
// 60 degrees, five times at 78. Where the ground is steep each layer is also
// read projected from the side, along world x and along world y at the same
// scale, and the three blended by the slope: the side projections take over
// between 45 and 62 degrees, split between themselves by which way the face
// looks. Ground flatter than 45 degrees (normal z at or above kCliffFlat)
// takes the stock read and nothing else, so it is the stock pixel.
const float kCliffFlat = 0.707;   // cos 45 degrees
const float kCliffSteep = 0.47;   // cos 62 degrees
const float kCliffSkip = 0.12;   // a read weighted under this is folded into the others
const float kCliffTexScale = 8.0 / 33.333333;
// x, y: the two side projections' weights; z: the top-down read's.
vec3 gCliffW = vec3(0.0, 0.0, 1.0);
vec2 gCliffUvX, gCliffUvY;
vec2 gCliffDxX, gCliffDyX, gCliffDxY, gCliffDyY, gCliffDxT, gCliffDyT;

void cliffSetup(vec3 n) {
    // Derivatives in uniform control flow, for the reads in the branches.
    // All three mappings are linear in the world position, so two
    // derivatives of it give all six.
    gCliffUvX = vec2(-FragPos.y, -FragPos.z) * kCliffTexScale;
    gCliffUvY = vec2(-FragPos.x, -FragPos.z) * kCliffTexScale;
    vec3 dPx = dFdx(FragPos) * kCliffTexScale;
    vec3 dPy = dFdy(FragPos) * kCliffTexScale;
    gCliffDxX = -dPx.yz; gCliffDyX = -dPy.yz;
    gCliffDxY = -dPx.xz; gCliffDyY = -dPy.xz;
    gCliffDxT = -dPx.yx; gCliffDyT = -dPy.yx;   // TexCoord is (-y, -x) * scale
    float steep = 1.0 - smoothstep(kCliffSteep, kCliffFlat, abs(n.z));
    if (steep <= 0.0) return;
    vec2 side = pow(abs(n.xy), vec2(8.0));
    side /= max(side.x + side.y, 1e-6);
    gCliffW = vec3(side * steep, 1.0 - steep);
    // At most two reads a layer where one would do: a small weight goes to
    // the largest of the others (the sum stays 1).
    if (gCliffW.x < kCliffSkip) { gCliffW.y += gCliffW.x; gCliffW.x = 0.0; }
    if (gCliffW.y < kCliffSkip) { gCliffW.x += gCliffW.y; gCliffW.y = 0.0; }
    if (gCliffW.z < kCliffSkip) {
        if (gCliffW.x >= gCliffW.y) gCliffW.x += gCliffW.z; else gCliffW.y += gCliffW.z;
        gCliffW.z = 0.0;
    }
}
#endif

// rgb the texture, a its specular mask (the `_s` alpha, else none).
vec4 layerTexel(sampler2D tex, int flags) {
    vec2 uv = TexCoord;
    if (chunkMode.z != 0) uv += layerScroll(flags);
#ifdef CLIFF_PROJECTION
    if (gCliffW.z < 1.0) {
        vec2 scroll = uv - TexCoord;
        vec4 c = vec4(0.0);
        if (gCliffW.z > 0.0) c += gCliffW.z * textureGrad(tex, uv, gCliffDxT, gCliffDyT);
        if (gCliffW.x > 0.0) c += gCliffW.x * textureGrad(tex, gCliffUvX + scroll, gCliffDxX, gCliffDyX);
        if (gCliffW.y > 0.0) c += gCliffW.y * textureGrad(tex, gCliffUvY + scroll, gCliffDxY, gCliffDyY);
        c.a = (flags & 0x10000) != 0 ? c.a : 0.0;
        return c;
    }
#endif
    vec4 t = texture(tex, uv);
    t.a = (flags & 0x10000) != 0 ? t.a : 0.0;
    return t;
}

void main() {
    float fragDist = length(viewPos.xyz - FragPos);
#ifdef CLIFF_PROJECTION
    cliffSetup(normalize(Normal));
#endif

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
    // Settled snow (wet_surface.glsli): 0, and untouched, without snow.
    if (localLightTileScale.w > 0.0) {
        float snowed = snowCoverAmount(geomNorm, finalColor.rgb, FragPos);
        if (snowed > 0.0) finalColor.rgb = snowCoverAlbedo(finalColor.rgb, snowed);
    }

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
        shadow = sunShadow(FragPos, norm, lightDir2);
        // shadowParams.y is the player's shadow strength over the stock
        // 0.3; 1 at the default leaves the map's 0..1 as it is.
        shadow = mix(1.0, shadow, clamp(shadowParams.y / (1.0 - kStockShadowFloor), 0.0, 1.0));
    }

    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow, clamp(shadowParams.y / (1.0 - kStockShadowFloor), 0.0, 1.0));
    // The baked shadow against the live one, the darker of the two (the
    // stock shader's MIN): a tree the dynamic map already shades is not
    // shaded twice, and ground beyond its reach keeps the baked shadow.
    shadow = min(shadow, texture(uBakedShadow, LayerUV).r);

    // With the HDR scene (localLightMeta.z bit 32) the clamp lifts: a face
    // the sun and the sky together light past one keeps its hue and goes to
    // the tone map, instead of stopping flat in the brightest channel.
    // Enhanced's daylight (kEnhancedAmbient, 128): the ground lit as its
    // terrain shader lights it (docs/re/enhanced-lighting.md 6e) - the sky's
    // ambient by Enhanced's half-Lambert ramp toward the sun, and the shadow
    // on the sun's light alone, where stock darkens ambient and sun together
    // to 70% in shade. The ramp is held to flat ground's: level ground keeps
    // the ambient it had (the Barrens' noon road would take x1.08 of it), a
    // slope turned from the sun takes less and one turned to it more. What
    // the shade then lacks the bounce pass puts back where the sky and the
    // sunlit ground round it reach (ambient_occlusion.frag).
    bool enhancedGround = (localLightMeta.z & 128) != 0;
    vec3 groundAmbient = rtAmbient(rt, ambientColor.rgb);
    if (enhancedGround) {
        float ramp = mix(0.7, 1.1, 0.5 + 0.5 * clamp(dot(norm, lightDir2), -1.0, 1.0));
        float rampFlat = mix(0.7, 1.1, 0.5 + 0.5 * clamp(lightDir2.z, -1.0, 1.0));
        groundAmbient *= ramp / rampFlat;
    }
    vec3 light = groundAmbient + (enhancedGround ? shadow : 1.0) * diff * lightColor.rgb;
    if ((localLightMeta.z & 32) == 0) light = min(light, vec3(1.0));
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
    if (!enhancedGround) result *= kStockShadowFloor + (1.0 - kStockShadowFloor) * shadow;
    // The highlight, masked by the blended `_s` alpha and the shadow, added
    // after the floor and not doubled (terrain1.bls: spec * tex.a * lit).
    result += Specular * finalColor.a * shadow;

    result += localLightContribution(FragPos, norm, finalColor.rgb);
    // The film over the ground is smoother than the ground: half the bump.
    if (wet > 0.0) {
        result += wetSurfaceSheen(normalize(mix(geomNorm, norm, 0.5)), FragPos, fragDist,
                                  wet, shadow);
    }
    // Rain ripples in the puddles (wet_surface.glsli); rtParams.z is 0 while
    // dry or with the setting off.
    if (rtParams.z > 0.0) {
        float rippleWidth = rainRippleWidth(FragPos.xy);
        if (wet > 0.0) result += wetGroundRipples(geomNorm, FragPos, fragDist, wet, rippleWidth);
    }

    result = applyFog(result, FragPos, fragDist);

#ifdef STREAM_FADE
    if (streamFadeHidden(fadePush.streamFade.x)) discard;
#endif
    outColor = vec4(result, 1.0);
}
