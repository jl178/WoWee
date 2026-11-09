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
#include "stream_fade.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    float alphaRef;   // the blend's alpha reference (0x873ee0); 0 = no test
    int unlit;
    int lightModeA;   // stock light mode (0x7a8b10): 0 none, 1 outdoor, 2 F_WINDOW, 3 MOHD ambient
    float specularIntensity;
    int lightModeB;   // a trans batch's room pass (0 or 3); -1 otherwise
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int isLava;
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
    int emissive;
    int hasVertexColors;
    uint sidnColor;   // F_SIDN night glow, 0x00RRGGBB; 0 = none
    int stockFlags;   // 1 unified (MapObjU), 2 F_UNFOGGED, 4 alpha from texture, 8 extras, 16 shadowed
    int effect;       // 0 Diffuse 1 Specular 2 Metal 3 Env 4 Opaque 5 EnvMetal 6 Composite
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;
// MOMT texture2: the env map of MapObjEnv / EnvMetal, the second layer of
// MapObjUComposite. White when the batch has none.
layout(set = 1, binding = 3) uniform sampler2D uTexture2;

#include "shadow_filter.glsli"
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;
layout(location = 6) flat in float SidnNight;
layout(location = 7) in vec4 Layer2;
layout(location = 8) in vec2 EnvCoord;
layout(location = 9) flat in float StreamHide;  // stream_fade.glsli

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
    // Torch and lamp light off: the stock rule, the 4 nearest M2 lights.
    if ((localLightMeta.z & 4) == 0) return albedo * stockPointLights(pos, normal, 4);
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
        vec3 lightVector = toLight / max(dist, 0.001);
        float attenuation = 1.0 - dist / radius;
        attenuation *= attenuation;
        float wrappedDiffuse = 0.22 + 0.78 * max(dot(normal, lightVector), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb *
               (localLightColorIntensity[i].w * attenuation * wrappedDiffuse);
    }
    }
    return sum;
}

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
}

// Where the parallax trace runs in full, and where it has eased to nothing.
const float kPomFullYards = 25.0;
const float kPomNoneYards = 45.0;

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor, float depthScale) {
    float VdotN = abs(viewDirTS.z);  // 1=head-on, 0=grazing

    // Fade out POM at grazing angles to avoid distortion
    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    // Direction to shift UV per layer - clamp denominator to prevent explosion at grazing angles
    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * depthScale;
    // Hard-clamp total UV offset to prevent texture swimming
    float maxOffset = depthScale * 3.0;
    P = clamp(P, vec2(-maxOffset), vec2(maxOffset));
    vec2 deltaUV = P / float(numSamples);

    // The mip level is chosen once, from the undisplaced UV, and used for
    // every sample in the march. Inside the loop the UV differs from one
    // pixel to the next by how many steps each has taken, so the implicit
    // derivatives were noise and the level chosen from them was too - which
    // showed as sparkle on relief at distance and cost a gradient fetch
    // per sample on top.
    float lod = textureQueryLod(uNormalHeightMap, uv).x;
    vec2 currentUV = uv;
    float currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;

    // Ray march through layers
    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    // Interpolate between last two layers for smooth result
    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    // Fade toward original UV at grazing angles for smooth transition
    float fadeFactor = smoothstep(0.15, 0.35, VdotN);
    return mix(uv, result, fadeFactor);
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

void main() {
    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    // Compute final UV (with POM if enabled)
    vec2 finalUV = TexCoord;

    // Lava/magma: scroll UVs for flowing effect
    if (isLava != 0) {
        float time = fogParams.z;
        // Scroll both axes - pools get horizontal flow, waterfalls get vertical flow
        // (UV orientation depends on mesh, so animate both)
        finalUV += vec2(time * 0.04, time * 0.06);
    }

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    // How many yards one repeat of the texture covers, from the same
    // derivatives. The parallax depth and the bump are both in texture space,
    // so a rock face whose texture is stretched thirty yards across got thirty
    // times the depth of a brick wall's - a mountain's surface sliding about
    // under the camera. Both are held to what a few yards of texture gets.
    vec3 posDx = dFdx(FragPos);
    vec3 posDy = dFdy(FragPos);
    float uvLen = max(length(uvDx) + length(uvDy), 1e-6);
    float yardsPerRepeat = (length(posDx) + length(posDy)) / uvLen;
    // The authored 0.012 is a depth of about five centimetres at 4 yards a
    // repeat; deeper than that only on smaller textures.
    float pomScaleHere = min(pomScale, 0.05 / max(yardsPerRepeat, 1e-3));
    float bumpScaleHere = mix(1.0, 0.35, smoothstep(8.0, 32.0, yardsPerRepeat));

    // The relief is a few centimetres deep: past 25 yards its shift is a
    // pixel or two, and past 45 none, so the trace eases out over that span
    // rather than running on every far wall - which is where most of its
    // cost on screen was (enhancements-plan.md 3.11).
    float pomFade = 1.0 - smoothstep(kPomFullYards, kPomNoneYards, length(viewPos.xyz - FragPos));
    if (enablePOM != 0 && heightMapVariance > 0.001 && lodFactor < 0.99 && pomFade > 0.0) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor, pomScaleHere * pomFade);
    }

    vec4 texColor = hasTexture != 0 ? textureGrad(uTexture, finalUV, uvDx, uvDy) : vec4(1.0);
    // MapObjComposite: the two layers blended by the second MOCV's alpha,
    // colour and alpha alike, before the light.
    if (effect == 6) {
        vec4 t1 = texture(uTexture2, Layer2.xy);
        texColor = mix(t1, texColor, clamp(Layer2.z, 0.0, 1.0));
    }

    // The pixel programs' alpha: MapObjDiffuse and Composite write vertex
    // alpha times texture alpha, the rest the vertex alpha alone. A trans
    // batch's vertex alpha is its doorway blend, used up below. The alpha
    // test is the blend's reference against that (0x873ee0): AlphaKey
    // 224/255, the blended modes 1/255, a trans batch none.
    float vertA = lightModeB >= 0 ? 1.0 : clamp(VertColor.a, 0.0, 1.0);
    float alpha = (stockFlags & 4) != 0 ? texColor.a * vertA : vertA;
    if (alphaRef > 0.0 && alpha < alphaRef) discard;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(normalMapStrength, 0.0, 1.0) * (1.0 - lodFactor) * bumpScaleHere;
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    // Sample the sun shadow map, for the draws the client binds it to
    // (stockFlags 16, 0x8745d0): a trans batch's outdoor pass and the
    // exterior groups of the no-MOCV and unified paths.
    float shadow = 1.0;
    if (shadowParams.x > 0.5 && (stockFlags & 16) != 0) {
        shadow = sunShadow(FragPos, vertexNormal, normalize(-lightDir.xyz));
        shadow = mix(1.0, shadow, shadowParams.y);
    }
    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow, shadowParams.y);

    // The stock light (Wow.exe 3.3.5a; stock_wmo_light.hpp has the same sums
    // on the CPU, and docs/re/world-lighting.md the addresses).
    //
    // The vertex programs MapObjDiffuse_T1 / MapObjUDiffuse_T1 light with
    // L = clamp(ambient + diffuse * clamp(N.L), 0, 1) in one of four modes
    // (0x7a8b10): 0 lighting off, 1 the outdoor light, 2 the F_WINDOW light
    // (the mean of the sky's diffuse and ambient, the ambient 16/255 brighter),
    // 3 the root's MOHD ambient alone. Then
    //     classic  2 * clamp(C * L + e / 2)
    //     unified  2 * clamp(L * 127/255 + C + e / 2)   (MOHD flag 0x2)
    //     mode 0   2 * C
    // with C the vertex colour as the loader fixed it (no MOCV: 0x7f7f7f, or
    // black on the unified path) and e the material's F_SIDN colour for the
    // hour. The pixel program multiplies the texture by that.
    //
    // A trans batch is two passes, the outdoor one (mode A) over the room one
    // (mode B), blended by the vertex alpha the portals gave it - so a doorway
    // fades from the room's baked light to the sky's.
    //
    // The sun shadow scales the outdoor pass by 0.7..1 (shadowParams.y = 0.3),
    // the way the client's shadow-map programs scale the whole lit colour.
    vec3 C = VertColor.rgb;
    bool unified = (stockFlags & 1) != 0;
    vec3 e = vec3(0.0);
    if (sidnColor != 0u) {
        vec3 sidn = vec3(float((sidnColor >> 16) & 0xFFu),
                         float((sidnColor >> 8) & 0xFFu),
                         float(sidnColor & 0xFFu)) / 255.0;
        float night = SidnNight;
        if (night < 0.0) {
            // Not told the hour: how dark the outdoor light is stands in.
            float daylight = clamp(dot(ambientColor.rgb + lightColor.rgb,
                                       vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
            night = 1.0 - smoothstep(0.25, 0.65, daylight);
        }
        e = sidn * floor(night * 255.0 + 0.0) / 256.0;
    }
    float nDotL = clamp(dot(norm, normalize(-lightDir.xyz)), 0.0, 1.0);
    vec3 skyAmbient = rtAmbient(rt, ambientColor.rgb);
    vec3 mohdAmbient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);

    bool hdrLight = (localLightMeta.z & 32) != 0;
    vec3 passA, passB = vec3(0.0);
    for (int pass = 0; pass < 2; ++pass) {
        int mode = pass == 0 ? lightModeA : lightModeB;
        if (mode < 0) break;
        vec3 c;
        if (mode == 0) {
            c = 2.0 * C;
        } else {
            vec3 L;
            if (mode == 1) {
                // Outdoor light, lifted with the HDR scene (bit 32) as the
                // terrain's; the doubled colour below is then left unclamped
                // too, for this mode only.
                L = max(skyAmbient + lightColor.rgb * nDotL, 0.0);
                if (!hdrLight) L = min(L, vec3(1.0));
            } else if (mode == 2) {
                vec3 mean = (lightColor.rgb + ambientColor.rgb) * 0.5;
                L = clamp(min(mean + 16.0 / 255.0, vec3(1.0)) + mean * nDotL, 0.0, 1.0);
            } else {
                L = clamp(mohdAmbient, 0.0, 1.0);
            }
            c = unified ? 2.0 * clamp(L * (127.0 / 255.0) + C + e * 0.5, 0.0, 1.0)
                        : 2.0 * clamp(C * L + e * 0.5, 0.0, (hdrLight && mode == 1) ? 65504.0 : 1.0);
        }
        // The shadow-map programs scale the whole colour, lit or not.
        if (pass == 0) c *= shadow;
        if (pass == 0) passA = c; else passB = c;
    }
    vec3 light = passA;
    if (lightModeB >= 0) {
        float w = clamp(VertColor.a, 0.0, 1.0);
        if ((stockFlags & 4) != 0) w *= texColor.a;
        light = mix(passB, passA, w);
    }

    // Rain-wet walls and paving (wet_surface.glsli), a Wowee extra: only on
    // surfaces lit by the outdoor light alone, and 0 - untouched - while dry
    // or with the setting off.
    float wet = (lightModeA == 1 && lightModeB < 0) ? wetSurfaceAmount(vertexNormal) : 0.0;
    vec3 albedo = wet > 0.0 ? wetSurfaceAlbedo(texColor.rgb, wet) : texColor.rgb;
    // Settled snow on roofs and paving, outdoor-lit surfaces only, as the wet.
    if (localLightTileScale.w > 0.0 && lightModeA == 1 && lightModeB < 0) {
        float snowed = snowCoverAmount(vertexNormal, albedo, FragPos);
        if (snowed > 0.0) albedo = snowCoverAlbedo(albedo, snowed);
    }
    vec3 result = albedo * light;
    // MapObjEnv adds texture alpha times the env map, MapObjEnvMetal texture
    // colour times its alpha times the env map; neither lit.
    if (effect == 3 || effect == 5) {
        vec3 env = texture(uTexture2, EnvCoord).rgb;
        result += (effect == 5 ? texColor.rgb : vec3(1.0)) * texColor.a * env;
    }
    if (wet > 0.0) {
        result += wetSurfaceSheen(normalize(mix(vertexNormal, norm, 0.5)), FragPos,
                                  length(viewPos.xyz - FragPos), wet, shadow);
    }
    // Rain ripples in the puddles on exterior paving (wet_surface.glsli; wet
    // is 0 indoors). rtParams.z is 0 while dry or with the setting off.
    if (rtParams.z > 0.0) {
        float rippleWidth = rainRippleWidth(FragPos.xy);
        if (wet > 0.0) {
            result += wetGroundRipples(vertexNormal, FragPos, length(viewPos.xyz - FragPos),
                                       wet, rippleWidth);
        }
    }

    // WOWEE_WMO_EXTRAS: non-stock looks picked by texture name.
    if (emissive == 1) {
        vec3 cell = floor(FragPos * 0.2);
        float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        float t = fogParams.z;
        float flicker = 0.93 + 0.05 * sin(t * 1.3 + h * 6.2831853)
                      + 0.02 * sin(t * 2.9 + h * 10.681);
        result = (texColor.rgb * 2.0 + vec3(0.16, 0.07, 0.015)) * flicker;
    } else if (emissive == 2) {
        float t = fogParams.z;
        float flicker = 0.84 + 0.10 * sin(t * 1.3) + 0.05 * sin(t * 2.9 + 1.7)
                      + 0.03 * sin(t * 6.7 + 0.6);
        float daylight = clamp(dot(ambientColor.rgb + lightColor.rgb,
                                   vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        result += vec3(1.0, 0.58, 0.22) * (0.30 * flicker * mix(1.0, 0.22, daylight));
    } else if (isLava != 0) {
        result = texColor.rgb * 1.5;
    }

    // Wowee's point lights (torches and the like): an extra the stock world
    // does not have, empty unless the renderer hands some in.
    if (isLava == 0)
        result += localLightContribution(FragPos, norm, texColor.rgb);

    // F_UNFOGGED (MOMT 0x2) materials are drawn with the fog off (0x7a8440).
    float dist = length(viewPos.xyz - FragPos);
    if ((stockFlags & 2) == 0) result = applyFog(result, FragPos, dist);

    // A building that has just streamed in, dissolving in (after every
    // texture read; see m2.frag).
    if (streamFadeHidden(StreamHide)) discard;
    outColor = vec4(result, alpha);
}
