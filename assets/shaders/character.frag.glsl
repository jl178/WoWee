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
#include "stock_fog.glsli"
#include "m2_combiner.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;
// The batch's second texture stage (white when it has one).
layout(set = 1, binding = 3) uniform sampler2D uTexture2;

layout(set = 1, binding = 1) uniform CharMaterial {
    float opacity;
    int alphaTest;
    int colorKeyBlack;
    int unlit;
    float emissiveBoost;
    // Keep these as scalar floats to match the C++ UBO packing. A std140 vec3
    // would insert padding here and shift the following material flags.
    float emissiveTintR;
    float emissiveTintG;
    float emissiveTintB;
    float specularIntensity;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int hairMaterial;
    // Stock unit-only ambient boost, 1 + 2 x AreaTable.AmbientMultiplier
    // (Wow.exe 0x783429). 0 reads as 1.
    float ambientMul;
    // Mouseover/target highlight (0x743C70): 1 adds the unit ambient as
    // emissive before the saturate.
    float highlight;
    // The stock pixel shader (m2_combiner.glsli), its texture stage count,
    // and the M2 blend mode.
    int combiner;
    int texStages;
    int blendMode;
    // The model's colour (CM2Model +0x180): scales the diffuse light only.
    float diffuseTintR;
    float diffuseTintG;
    float diffuseTintB;
    // Room light indoors (E12, not stock): probeW 0 leaves the stock light;
    // 1 + indoor gives this unit its own colour, indoor factor and lean.
    float probeR;
    float probeG;
    float probeB;
    float probeW;
    float probeLeanX;
    float probeLeanY;
    float probePad0;
    float probePad1;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

#include "shadow_filter.glsli"
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;
layout(location = 5) in vec2 TexCoord2;

layout(location = 0) out vec4 outColor;

const int PREVIEW_SIMPLE_TEXTURE_MODE = -31336;

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

// The M2 point lights on a unit, stock (CM2Light ctor 0x834A40 sets the
// attenuation to (0, 0.7, 0.03); diffuse_t1.bls lights per vertex with
// colour * max(N.L, 0) / (0.7 d + 0.03 d^2)). Stock takes the four nearest
// and has no range; the radius here only skips the far ones cheaply.
vec3 unitPointLights(vec3 pos, vec3 normal) {
    // Torch and lamp light off: stock, the four nearest M2 lights.
    if ((localLightMeta.z & 4) == 0) return stockPointLights(pos, normal, 4);
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
        float distSq = dot(toLight, toLight);
        if (radius <= 0.0 || distSq >= radius * radius) continue;
        float dist = sqrt(distSq);
        float ndl = max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += localLightColorIntensity[i].rgb * localLightColorIntensity[i].w * ndl /
               max(0.7 * dist + 0.03 * distSq, 0.05);
    }
    }
    return sum;
}


// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    return smoothstep(0.0001, 0.005, texelDensity);
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-8) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 n) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return safeNormalize(cross(axis, n), vec3(1.0, 0.0, 0.0));
}

bool finiteVec3(vec3 v) {
    return all(equal(v, v)) && all(lessThan(abs(v), vec3(1e10)));
}

bool isMagentaKeyColor(vec4 color) {
    return color.r >= 0.58 && color.b >= 0.58 && color.g <= 0.48 &&
           color.r >= color.g + 0.22 && color.b >= color.g + 0.22 &&
           abs(color.r - color.b) <= 0.38;
}

ivec2 wrapPreviewTexel(ivec2 texel, ivec2 texSize) {
    return ivec2((texel.x % texSize.x + texSize.x) % texSize.x,
                 (texel.y % texSize.y + texSize.y) % texSize.y);
}

vec4 samplePreviewTexture(sampler2D tex, vec2 uv) {
    ivec2 texSize = textureSize(tex, 0);
    if (texSize.x <= 0 || texSize.y <= 0) {
        return textureLod(tex, uv, 0.0);
    }

    vec2 wrappedUv = uv - floor(uv);
    ivec2 baseTexel = ivec2(floor(wrappedUv * vec2(texSize)));
    baseTexel = wrapPreviewTexel(baseTexel, texSize);

    vec4 color = texelFetch(tex, baseTexel, 0);
    if (!isMagentaKeyColor(color)) {
        return color;
    }

    for (int radius = 1; radius <= 4; ++radius) {
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                if (abs(x) != radius && abs(y) != radius) {
                    continue;
                }
                vec4 candidate = texelFetch(tex, wrapPreviewTexel(baseTexel + ivec2(x, y), texSize), 0);
                if (!isMagentaKeyColor(candidate)) {
                    return vec4(candidate.rgb, 0.0);
                }
            }
        }
    }

    return vec4(0.0);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor, float depthScale) {
    float VdotN = abs(viewDirTS.z);

    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * depthScale;
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

    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

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
// The world's curve (stock_fog.glsli): view depth and the zone's exponent.
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
    if (enablePOM == PREVIEW_SIMPLE_TEXTURE_MODE) {
        vec4 texColor = samplePreviewTexture(uTexture, TexCoord);
        if (isMagentaKeyColor(texColor)) {
            discard;
        }
        if (alphaTest != 0 && texColor.a < 0.5) {
            discard;
        }
        if (alphaTest != 0 && hairMaterial != 0) {
            texColor.a = 1.0;
        }
        if (colorKeyBlack != 0) {
            float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
            float ck = smoothstep(0.12, 0.30, lum);
            texColor.a *= ck;
            if (texColor.a < 0.01) discard;
        }
        // The stock combiner here too, so a preview keeps the shine its
        // folded layer used to draw on its own.
        vec4 previewTex2 = texStages > 1 ? texture(uTexture2, TexCoord2) : vec4(1.0);
        M2Combined pc = m2Combine(combiner, texColor, previewTex2, opacity);
        vec3 previewRgb = pc.mul * (blendMode == 6 ? 0.5 : 1.0) + pc.add;
        outColor = vec4(previewRgb, texColor.a * opacity);
        return;
    }

    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    vec2 finalUV = TexCoord;

    bool usePOM = enablePOM != 0 &&
                  alphaTest == 0 &&
                  colorKeyBlack == 0 &&
                  heightMapVariance > 0.001 &&
                  lodFactor < 0.99;
    // unlit: bit 0 = M2 render flag 0x1 (unlit), bit 1 = 0x2 (unfogged; set
    // for glue scenes, whose skies and cloud cards carry it).
    bool isUnlit = (unlit & 1) != 0;
    bool unfogged = (unlit & 2) != 0;
    bool useNormalMap = enableNormalMap != 0 &&
                        !isUnlit &&
                        lodFactor < 0.99 &&
                        normalMapStrength > 0.001;
    mat3 TBN;
    if (usePOM || useNormalMap) {
        vec3 T = safeNormalize(Tangent, fallbackTangent(vertexNormal));
        T = safeNormalize(T - dot(T, vertexNormal) * vertexNormal, fallbackTangent(vertexNormal));
        vec3 B = safeNormalize(Bitangent, safeNormalize(cross(vertexNormal, T), vec3(0.0, 1.0, 0.0)));
        TBN = mat3(T, B, vertexNormal);
    }

    // Eased out between 25 and 45 yards, as on buildings (wmo.frag).
    float pomFade = 1.0 - smoothstep(25.0, 45.0, length(viewPos.xyz - FragPos));
    if (usePOM && pomFade > 0.0) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor, pomScale * pomFade);
    }

    vec4 texColor = textureGrad(uTexture, finalUV, uvDx, uvDy);
    // Repair dark DXT fringes on alpha-cut character textures such as hair.
    // Transparent edge texels can carry black/garbage RGB even when alpha is
    // valid; pull color from a coarser mip and trust the source more as alpha
    // approaches opaque. This matches the generic M2 path.
    // Smooth leaf edges (enh_bits::kSmoothLeaves): the fringe repair and the
    // coverage ramp below. Off is the stock alpha key at 224/255.
    bool smoothLeaves = (localLightMeta.z & 16) != 0;
    if (smoothLeaves && alphaTest != 0 && texColor.a > 0.01 && texColor.a < 1.0) {
        vec3 mipColor = textureLod(uTexture, finalUV, 4.0).rgb;
        float trust = smoothstep(0.0, 0.9, texColor.a);
        texColor.rgb = mix(mipColor, texColor.rgb, trust);
    }

    // Some classic/TBC character textures use bright magenta as a color key.
    // Apply this before any material-specific alpha path because a few preview
    // batches report as opaque/blended even when their texture still carries
    // mask-color texels.
    if (texColor.r > 0.78 && texColor.g < 0.28 && texColor.b > 0.78) {
        discard;
    }

    if (alphaTest != 0 && hairMaterial != 0) {
        if (texColor.a < 0.5) {
            discard;
        }
        texColor.a = 1.0;
    } else if (alphaTest != 0 && !smoothLeaves) {
        if (texColor.a < 224.0 / 255.0) discard;
        texColor.a = 1.0;
    } else if (alphaTest != 0) {
        // Screen-space sharpened alpha for alpha-to-coverage anti-aliasing.
        // Rescales alpha so the 0.5 cutoff maps to exactly the texel boundary,
        // giving smooth edges when MSAA + alpha-to-coverage is active.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - 0.5) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    }
    if (colorKeyBlack != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        float ck = smoothstep(0.12, 0.30, lum);
        texColor.a *= ck;
        if (texColor.a < 0.01) discard;
    }

    // The stock combiner (m2_combiner.glsli): what the lit material colour
    // multiplies, and what is added unlit after it - an armour or elemental
    // env shine on its second stage, say.
    vec4 texColor2 = texStages > 1 ? texture(uTexture2, TexCoord2) : vec4(1.0);
    M2Combined cmb = m2Combine(combiner, texColor, texColor2, opacity);
    bool modBlend = (blendMode == 5 || blendMode == 6);

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (useNormalMap) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal.xy *= normalMapStrength;
        mapNormal = safeNormalize(mapNormal, vec3(0.0, 0.0, 1.0));
        vec3 worldNormal = safeNormalize(TBN * mapNormal, vertexNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        float blendFactor = max(lodFactor, 1.0 - normalMapStrength);
        norm = safeNormalize(mix(worldNormal, vertexNormal, blendFactor), vertexNormal);
    }

    vec3 result;

    if (modBlend) {
        // Mod and Mod2x: diffuse 0, emissive 1 or 0.5 (0x81FE90) - no light.
        result = cmb.mul * (blendMode == 6 ? 0.5 : 1.0);
    } else if (isUnlit) {
        vec3 emissiveTint = vec3(emissiveTintR, emissiveTintG, emissiveTintB);
        vec3 warm = emissiveTint * emissiveBoost;
        result = cmb.mul * (1.0 + warm);
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        float shadow = 1.0;
        if (shadowParams.x > 0.5) {
            shadow = sunShadow(FragPos, vertexNormal, ldir);
            shadow = mix(1.0, shadow, shadowParams.y);
        }
        RtLight rt = rtLightAt(FragPos);
        shadow = rtShadow(rt, shadow, shadowParams.y);

        // Stock unit light (CM2Lighting, docs/re/characters.md section 1):
        //   lit = clamp(ambient + diffuse * max(N.L, 0) + points, 0, 1)
        //   colour = texture * clamp(lit + emissive, 0, 1)
        // No specular, no overbright.
        float amul = ambientMul > 0.0 ? ambientMul : 1.0;
        // Outdoors: the world's ambient and diffuse bands and the sun, the
        // ambient boosted for units and clamped (0x7C1730, flag 0x8000).
        vec3 outAmb = rtAmbient(rt, min(ambientColor.rgb * amul, vec3(1.0)));
        vec3 diffuseTint = vec3(diffuseTintR, diffuseTintG, diffuseTintB);
        vec3 outLit = outAmb + shadow * diff * lightColor.rgb * diffuseTint;
        // Indoors (0x7C1AD0): the building's colour split into a diffuse
        // whose brightest channel is raised to 168/255 and an ambient whose
        // brightest channel is capped at 96/255, lit from the fixed
        // direction (-0.308, -0.308, -0.9) (0xAEEDF0). The sun is not
        // switched off, it is replaced. Stock samples the MOCV under each
        // unit; Wowee has the group's colour, handed over in the .w slots.
        vec3 ic = vec3(lightDir.w, lightColor.w, ambientColor.w);
        float m = max(max(ic.r, ic.g), ic.b);
        vec3 inDiffC = (m > 0.0 && m < 168.0 / 255.0) ? ic * ((168.0 / 255.0) / m) : ic;
        vec3 inAmb = (m > 96.0 / 255.0) ? ic * ((96.0 / 255.0) / m) : ic;
        inAmb = rtAmbient(rt, min(inAmb * amul, vec3(1.0)));
        float inN = max(dot(norm, normalize(vec3(0.30822, 0.30822, 0.9))), 0.0);
        vec3 inLit = inAmb + inN * inDiffC * diffuseTint;
        // shadowParams.w: the eased indoor factor (stock eases the ambient
        // over 0.5 s and snaps the diffuse; one factor here).
        float indoor = shadowParams.w;
        if (probeW > 0.5) {
            // Room light indoors (E12, indoor_probes.hpp): the same split of
            // this unit's own room colour, the fixed direction leaned a
            // little toward the brighter side of the room, and a faint fill
            // of the room's ambient on the side turned from it (kFill 0.35).
            vec3 pc = vec3(probeR, probeG, probeB);
            float pm = max(max(pc.r, pc.g), pc.b);
            vec3 pDiffC = (pm > 0.0 && pm < 168.0 / 255.0) ? pc * ((168.0 / 255.0) / pm) : pc;
            vec3 pAmb = (pm > 96.0 / 255.0) ? pc * ((96.0 / 255.0) / pm) : pc;
            pAmb = rtAmbient(rt, min(pAmb * amul, vec3(1.0)));
            vec3 pDir = normalize(vec3(0.30822 + probeLeanX, 0.30822 + probeLeanY, 0.9));
            float pN = max(dot(norm, pDir), 0.0);
            inLit = pAmb * (1.0 + 0.35 * (1.0 - pN)) + pN * pDiffC * diffuseTint;
            indoor = probeW - 1.0;
        }
        vec3 lit = mix(outLit, inLit, indoor);
        lit += unitPointLights(FragPos, norm);
        lit = clamp(lit, 0.0, 1.0);
        // The highlight is the world unit ambient, added as emissive.
        lit = clamp(lit + highlight * ambientColor.rgb, 0.0, 1.0);
        result = cmb.mul * lit;
    }
    result += cmb.add;

    float dist = length(viewPos.xyz - FragPos);
    if (!unfogged) {
        if (blendMode >= 3 && blendMode <= 6) {
            // Fog toward what leaves the framebuffer as it is (0xA45390):
            // black for the additive modes, white for Mod, grey for Mod2x.
            vec3 neutral = blendMode == 5 ? vec3(1.0) : (blendMode == 6 ? vec3(0.5) : vec3(0.0));
            result = mix(neutral, result, stockFogVisibility(FragPos));
            if (volumetricParams.x > 0.5) result = mix(neutral, result, fogVolumeAt(FragPos).a);
        } else {
            result = applyFog(result, FragPos, dist);
        }
    }
    if (!finiteVec3(result)) {
        result = texColor.rgb;
    }

    // A cutout's alpha is its sharpened coverage, as before; otherwise the
    // combiner's alpha, which for an opaque stage is the batch alpha alone.
    float outAlpha = alphaTest != 0 ? texColor.a * opacity : cmb.aMul * opacity + cmb.aAdd;
    if (colorKeyBlack != 0 && alphaTest == 0) outAlpha = texColor.a * opacity;
    if (blendMode >= 2 && alphaTest == 0 && outAlpha < 1.0 / 255.0) discard;
    outColor = vec4(result, outAlpha);
}
