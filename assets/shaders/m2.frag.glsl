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
#include "stream_fade.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;
// The batch's second texture stage (white when it has one stage).
layout(set = 1, binding = 3) uniform sampler2D uTexture2;

layout(set = 1, binding = 2) uniform M2Material {
    int hasTexture;
    int alphaTest;
    int colorKeyBlack;
    float colorKeyThreshold;
    int unlit;
    int blendMode;
    float fadeAlpha;
    float interiorDarken;
    float specularIntensity;
    float emissiveBoost;
    float tintR;
    float tintG;
    float tintB;
    int volumetricBeam;
    int fireCard;
    // The stock pixel shader (m2_combiner.glsli) and how many texture stages
    // it reads.
    int combiner;
    int texStages;
};

#include "shadow_filter.glsli"
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) flat in vec3 InstanceOrigin;
layout(location = 4) in float ModelHeight;
layout(location = 5) in float vFadeAlpha;
layout(location = 6) flat in int vSkyMode;
layout(location = 7) flat in float vHighlight;
layout(location = 8) in vec2 TexCoord2;
// A doodad of an interior WMO group (0x7c1150): x its ambient, y its
// diffuse (packed RGBA8, from the MODD colour by 0x7c1ad0), z 1 when set.
layout(location = 9) flat in uvec3 vWmoLight;
layout(location = 10) flat in float vStreamHide;  // stream_fade.glsli
layout(location = 11) flat in vec3 vInstanceTint;  // animated M2Color RGB, or -1

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
        float attenuation = 1.0 - dist / radius;
        attenuation *= attenuation;
        float wrappedDiffuse = 0.22 + 0.78 * max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb *
               (localLightColorIntensity[i].w * attenuation * wrappedDiffuse);
    }
    }
    return sum;
}

// 4x4 Bayer dither matrix (normalized to 0..1)
float bayerDither4x4(ivec2 p) {
    int idx = (p.x & 3) + (p.y & 3) * 4;
    float m[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    return m[idx];
}

/// Value noise on a world position, for the haze inside a light beam.
///
/// Trilinear between eight hashed lattice corners with a smoothstep fade, so
/// it is continuous and has no visible grid. Sampled in world space by the
/// caller, which is what keeps the mist still while the beam sweeps over it.
float beamHash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float beamHaze(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(beamHash(i + vec3(0, 0, 0)), beamHash(i + vec3(1, 0, 0)), f.x),
                   mix(beamHash(i + vec3(0, 1, 0)), beamHash(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(beamHash(i + vec3(0, 0, 1)), beamHash(i + vec3(1, 0, 1)), f.x),
                   mix(beamHash(i + vec3(0, 1, 1)), beamHash(i + vec3(1, 1, 1)), f.x), f.y), f.z);
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

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

void main() {
    vec4 texColor = hasTexture != 0 ? texture(uTexture, TexCoord) : vec4(1.0);
    vec4 texColor2 = texStages > 1 ? texture(uTexture2, TexCoord2) : vec4(1.0);
    // The batch's authored colour. A glow card is painted white and coloured
    // here - Orgrimmar's bonfire carries (1.0, 0.329, 0.0) - so without it
    // every fire in the world burns white. Stock puts it in the material
    // colour (0x81FE90), which the combiner multiplies by the lit part only;
    // Mod and Mod2x batches take none (diffuse 0, emissive 1 or 0.5).
    bool modBlend = (blendMode == 5 || blendMode == 6);
    vec3 tint = modBlend ? vec3(blendMode == 6 ? 0.5 : 1.0)
              : (vInstanceTint.x >= 0.0 ? vInstanceTint : vec3(tintR, tintG, tintB));

    // Original client sky M2s carry their authored colour and alpha, and are
    // taken as they are. They are camera-centered and unlit, and must not be
    // swallowed by world-distance fog.
    //
    // This used to sit below the three discards, which meant the sky was not
    // taken as it is. The alpha test in particular rescales alpha by its own
    // screen-space derivative:
    //
    //     float aGrad = fwidth(texColor.a);
    //     texColor.a = clamp((texColor.a - alphaCutoff) / max(aGrad, 0.001) ...
    //
    // fwidth is how fast alpha changes from one pixel to the next, so it
    // changes whenever the view does - and a nebula's alpha ramp is gentle,
    // which makes the divisor tiny and the result a hard edge. Turning the
    // camera moved that edge, so Hellfire's sky flickered while the view moved
    // and stood still when it did not, on its blended layers alone. The rescale
    // is for foliage cutouts, where a hard edge is the point.
    if (vSkyMode != 0) {
        // An opaque layer's alpha channel says nothing - it was never
        // blended - so only the fade is its alpha. That matters while the
        // sky crossfades between zones, when every layer is drawn blended.
        M2Combined skyCmb = m2Combine(combiner, texColor, texColor2, vFadeAlpha);
        float skyAlpha = skyCmb.aMul * vFadeAlpha + skyCmb.aAdd;
        // Behind all the air there is, as the procedural sky is. An
        // additive layer only loses what the air hides of it; the air's own
        // light is already in the layer it is added to.
        vec3 skyColor = skyCmb.mul * tint + skyCmb.add;
        if (volumetricParams.x > 0.5) {
            vec4 clip = projection * view * vec4(FragPos, 1.0);
            vec4 air = fogVolumeSky(clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5);
            skyColor = (blendMode >= 3) ? skyColor * air.a : skyColor * air.a + air.rgb;
        }
        outColor = vec4(skyColor, skyAlpha);
        return;
    }

    bool isFoliage = (alphaTest == 2);

    // Fix DXT fringe: transparent edge texels have garbage (black) RGB, and
    // bilinear drags it into the leaf's edge. The blurred high mip is the
    // colour the edge should have been.
    //
    // Only where the fringe actually is. This used to run on everything below
    // full alpha and trust the texel in proportion to it, so a leaf texel at
    // half alpha - which is leaf, not fringe, and the canopy is full of them -
    // came out sixty percent a blurred sixteen-pixel average of the whole
    // sheet. That is what took the contrast and the colour out of a lit
    // canopy and left it looking milky. A quarter alpha is where the fringe
    // stops and the leaf starts.
    // Smooth leaf edges (enh_bits::kSmoothLeaves) is everything from here to
    // the sharpened alpha: the fringe repair, the mip boost and the coverage
    // ramp. Off is the stock alpha key: reference 224/255 (0x81FE90).
    bool smoothLeaves = (localLightMeta.z & 16) != 0;
    if (alphaTest != 0 && !smoothLeaves) {
        if (texColor.a < 224.0 / 255.0) discard;
        texColor.a = 1.0;
    }
    if (smoothLeaves && alphaTest != 0 && texColor.a > 0.01 && texColor.a < 0.25) {
        vec3 mipColor = textureLod(uTexture, TexCoord, 4.0).rgb;
        float trust = smoothstep(0.0, 0.25, texColor.a);
        texColor.rgb = mix(mipColor, texColor.rgb, trust);
    }

    float alphaCutoff = 0.5;
    if (alphaTest == 2) {
        alphaCutoff = 0.4;
    } else if (alphaTest == 3) {
        alphaCutoff = 0.25;
    } else if (alphaTest != 0) {
        alphaCutoff = 0.4;
    }
    // Mip-alpha preservation: alpha mips average downward, thinning distant
    // canopies to skeletons. Boost alpha with mip level so perceived leaf
    // density stays constant with distance.
    if (smoothLeaves && isFoliage && hasTexture != 0) {
        // Gentler than it was: at 0.18 a mip-4 canopy came back with every
        // leaf texel above 0.23 alpha opaque, which is a solid green mass
        // rather than a thinned one. Coverage carries the rest now.
        float mip = textureQueryLod(uTexture, TexCoord).x;
        texColor.a *= 1.0 + clamp(mip, 0.0, 4.0) * 0.11;
    }
    if (smoothLeaves && alphaTest != 0) {
        // Screen-space sharpened alpha: rescale so the cutoff maps to the
        // texel boundary. With MSAA + alpha-to-coverage on the cutout
        // pipeline this dithers the edge band across samples, smoothing
        // leaf silhouettes instead of the old hard binary discard.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - alphaCutoff) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    }
    if (colorKeyBlack != 0) {
        float lum = dot(texColor.rgb * tint, vec3(0.299, 0.587, 0.114));
        if (lum < colorKeyThreshold) discard;
    }
    if (blendMode == 1 && texColor.a < 0.004) discard;

    // Per-instance color variation (foliage only).
    //
    // Half what it was. A thirty-point spread in brightness is a third of the
    // way to another time of day, and two canopies of the same tree standing
    // in each other reads as a seam down the middle of the crown rather than
    // as two trees. Enough to break the copy, not enough to be seen as a
    // difference in light.
    if (false) {  // the stock client tints no tree; kept for reference
        float hash = fract(sin(dot(InstanceOrigin.xy, vec2(127.1, 311.7))) * 43758.5453);
        float hueShiftR = 1.0 + (hash - 0.5) * 0.10;       // ±5% red
        float hueShiftB = 1.0 + (fract(hash * 7.13) - 0.5) * 0.10; // ±5% blue
        float brightness = 0.93 + hash * 0.14;               // 93-107%
        texColor.rgb *= vec3(hueShiftR, 1.0, hueShiftB) * brightness;
    }

    vec3 norm = normalize(Normal);
    bool foliageTwoSided = (alphaTest == 2);
    if (!foliageTwoSided && !gl_FrontFacing) norm = -norm;
    // Ground clutter is crossed vertical cards; lit by their own normals they
    // caught the low sun twice as well as the ground. Shade them as the
    // terrain under them, as the stock client does.
    bool groundDetail = (alphaTest == 3);
    if (groundDetail) {
        // The terrain normal the vertex stage handed over for clutter the
        // ground scatterer placed (unflipped: a card's back face is still the
        // same ground). A card's own normal is near horizontal, so anything
        // that steep is not a ground normal and falls back to straight up.
        vec3 g = normalize(Normal);
        norm = g.z > 0.45 ? g : vec3(0.0, 0.0, 1.0);
    }

    // Detail normal perturbation (foliage only) - UV-based only so wind doesn't cause flicker
    if (isFoliage) {
        float nx = sin(TexCoord.x * 12.0 + TexCoord.y * 5.3) * 0.10;
        float ny = sin(TexCoord.y * 14.0 + TexCoord.x * 4.7) * 0.10;
        norm = normalize(norm + vec3(nx, ny, 0.0));
    }

    // The stock combiner: what the lit material colour multiplies (albedo)
    // and what is added unlit after it (an env shine, say). Taken after the
    // cutout work above, which reads and sharpens the first stage's alpha.
    M2Combined cmb = m2Combine(combiner, texColor, texColor2, vFadeAlpha);
    vec3 albedo = cmb.mul * tint;

    // An interior WMO doodad is lit by its own two colours along a fixed
    // direction (0xaeedf0), not by the day/night light.
    bool wmoInterior = vWmoLight.z != 0u;
    vec3 ldir = wmoInterior ? normalize(vec3(0.30822, 0.30822, 0.9))
                            : normalize(-lightDir.xyz);
    vec3 sunColor = wmoInterior ? unpackUnorm4x8(vWmoLight.y).rgb : lightColor.rgb;
        float nDotL = dot(norm, ldir);
        float diff = max(nDotL, 0.0);  // a leaf's back stays dark

    vec3 result;
    if (modBlend) {
        // Mod and Mod2x: diffuse 0, emissive 1 or 0.5 (0x81FE90), so the
        // colour is the combiner's output against a constant - no light.
        result = albedo;
    } else if (unlit != 0) {
        result = albedo * emissiveBoost;
        if (emissiveBoost > 1.0) {
            // Weighted by the texel's own brightness. Added flat it lit the
            // whole quad, and a glow card is black everywhere but its middle,
            // so the card's rectangle appeared as an orange panel hanging on
            // whatever was behind the fire. Black has nothing to boost.
            float emissiveWeight =
                dot(albedo, vec3(0.299, 0.587, 0.114));
            result += vec3(0.32, 0.14, 0.025) * (emissiveBoost - 1.0) *
                      emissiveWeight;
        }
    } else {
        vec3 viewDir = normalize(viewPos.xyz - FragPos);

        float spec = 0.0;
        float shadow = 1.0;
        if (!isFoliage && !groundDetail && !wmoInterior) {
            vec3 halfDir = normalize(ldir + viewDir);
            spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;
        }

        if (shadowParams.x > 0.5 && !wmoInterior) {
            shadow = sunShadow(FragPos, norm, ldir);
            shadow = mix(1.0, shadow, shadowParams.y);
        }

        // Leaf subsurface scattering (foliage only) - uses stable normal, no FragPos dependency
        vec3 sss = vec3(0.0);
        if (isFoliage) {
            float backLit = max(-nDotL, 0.0);
            float viewDotLight = max(dot(viewDir, -ldir), 0.0);
            float sssAmount = backLit * pow(viewDotLight, 4.0) * 0.35 * texColor.a;
            sss = sssAmount * vec3(1.0, 0.9, 0.5) * lightColor.rgb;
        }

        // Sky-bounce ambient for foliage: upward-facing leaves catch more
        // ambient than the canopy underside, giving the crown depth instead
        // of a uniformly-lit blob.
        RtLight rt = rtLightAt(FragPos);
        if (!wmoInterior) shadow = rtShadow(rt, shadow, shadowParams.y);
        // The area's ambient as the terrain, the WMO walls and the characters
        // take it. Foliage used to take 82-100% of it by how far the leaf
        // faced up, a darkening that dates from when the outdoor ambient
        // carried an extra +0.4 from LightingManager's defaults; against the
        // true Light.dbc blend it left trees darker than the ground they
        // stand on. The stock client lights foliage like any other M2.
        vec3 ambientTerm = wmoInterior ? unpackUnorm4x8(vWmoLight.x).rgb
                                       : rtAmbient(rt, ambientColor.rgb);
        // Clamped at one per channel, as the stock vertex shaders clamp
        // ambient + diffuse * N.L before the texture (shaders/vertex/arbvp1/
        // diffuse_t1.bls: MIN R0, R0, 1).
        vec3 light = min(ambientTerm + shadow * diff * sunColor, vec3(1.0));
        result = light * albedo + shadow * spec * sunColor;
        // No leaf glow (sss unused): stock foliage is lit like any other M2.

        if (interiorDarken > 0.0) {
            result *= mix(1.0, 0.5, interiorDarken);
        }
    }

    // Canopy ambient occlusion (foliage only). ModelHeight arrives as the
    // fraction of the plant's own height, so the shading sits in the same
    // place on a forty-yard tree and on a knee-high fern; it used to be raw
    // model-space z against a hardcoded eighteen, which put the darkening on
    // the trunk of anything tall and over the whole of anything short.
    //
    // Off: the stock client has no canopy occlusion, and it was tuned against
    // the over-bright outdoor ambient (see the ambient term above). Halving
    // the lower part of every plant made Elwynn's trees and Stormwind's
    // planters darker than the walls and street around them.
    if (false && isFoliage) {
        float aoFactor = mix(0.55, 1.0, smoothstep(0.0, 0.6, ModelHeight));
        result *= aoFactor;
    }

    if (unlit == 0 && !modBlend) result += localLightContribution(FragPos, norm, albedo);
    // The combiner's unlit part (Opaque_AddNA's env shine and the like).
    result += cmb.add;

    float dist = length(viewPos.xyz - FragPos);
    if (modBlend) {
        // Mod and Mod2x fog toward the colour that leaves the framebuffer as
        // it is: white for Mod, grey for Mod2x (table 0xA45390).
        vec3 neutral = vec3(blendMode == 6 ? 0.5 : 1.0);
        result = mix(neutral, result, stockFogVisibility(FragPos));
        if (volumetricParams.x > 0.5) result = mix(neutral, result, fogVolumeAt(FragPos).a);
    } else if (blendMode >= 3) {
        // Additive. Mixing toward the fog colour would give the card's black
        // corners the fog's colour, and additive then adds that to the scene -
        // the whole quad shows up as a lit rectangle hanging in the air, which
        // is what Orgrimmar's bonfire glow was doing to the wall behind it.
        // Distance can only take an additive contribution away, and so can
        // the air in front of it: its own light is already in the scene
        // behind the card.
        float fogFactor = stockFogVisibility(FragPos);
        result *= fogFactor;
        if (volumetricParams.x > 0.5) result *= fogVolumeAt(FragPos).a;
    } else {
        result = applyFog(result, FragPos, dist);
    }

    float outAlpha = cmb.aMul * vFadeAlpha + cmb.aAdd;
    // Cutout materials output the sharpened coverage alpha computed above -
    // alpha-to-coverage turns it into per-sample coverage for smooth edges.
    // Color-key-only materials have no meaningful texture alpha; keep them
    // opaque after the discard.
    if (colorKeyBlack != 0 && alphaTest == 0) {
        outAlpha = vFadeAlpha;
    }
    // Stock's alpha reference for every blended mode is 1/255 (0x81FE90):
    // a texel with no alpha leaves the framebuffer alone, whatever its colour.
    if (blendMode >= 2 && alphaTest == 0 && colorKeyBlack == 0 && outAlpha < 1.0 / 255.0) discard;
    // The distance fade, for a batch drawn with no blending to fade through.
    // Sixteen ordered steps against the fragment's own screen position: a tree
    // at the edge of the draw distance thins out rather than switching off,
    // and it reads the same at every sample count, where leaving the fade in
    // the coverage alone would give two steps at 2x MSAA and none with MSAA
    // off. Multiplying it into the coverage as well would take the leaf edges
    // with it, so the cutout keeps its own alpha.
    if (alphaTest != 0 && blendMode <= 1) {
        if (vFadeAlpha <= bayerDither4x4(ivec2(gl_FragCoord.xy))) discard;
        outAlpha = texColor.a;
    }
    // Pressed on. The real client lifts the whole model while the button is
    // down over it, which is what says "this one, and the click landed": a
    // warm brightening rather than a tint, so a dark door reads as lit and a
    // pale one does not blow out.
    if (vHighlight > 0.0) {
        float lift = clamp(vHighlight, 0.0, 1.0);
        result = result * (1.0 + 0.6 * lift) + vec3(0.22, 0.19, 0.10) * lift;
    }

    // A shaft of light has no edge, and it is full of the air it lights.
    //
    // A searchlight, a god ray and a window shaft are all drawn as geometry -
    // a card or a flattened cone - and the geometry ends somewhere. Where it
    // ends the light stops dead, so the zeppelin's searchlight over Tirisfal
    // came to a straight bright line across the sky with the polygon's corner
    // plainly visible.
    //
    // Four attenuations soften it: the card's border, the cone's silhouette,
    // haze through the volume, and distance from the emitter. All four are
    // needed because no one of them reaches every beam - see each below.
    //
    // Applied to the whole model rather than only to batches whose blend mode
    // reads additive. That guard was here to spare the lamp housing and is
    // what kept the effect off the thing it was written for: the searchlight's
    // batches carry a raw blend mode of 0 or 1 and are turned additive later,
    // by the pass that draws them.
    if (volumetricBeam != 0) {
        vec2 fromEdge = min(TexCoord, vec2(1.0) - TexCoord);
        float border = min(fromEdge.x, fromEdge.y);
        float t = fogParams.z;
        vec3 drift = vec3(0.02, 0.013, -0.007) * t;

        // The edge, eaten into by the same air.
        //
        // A smooth falloff still reads as an edge, because it is the same
        // width the whole way along and perfectly straight. Real light gives
        // out against whatever is floating in front of it, so the boundary
        // wanders. This pushes the falloff in and out with a finer octave of
        // the same world-anchored field the haze uses - finer, so the edge
        // frays rather than scallops, and world-anchored for the same reason
        // as the haze: the fraying belongs to the air, so it must not travel
        // with the beam.
        float edgeNoise = beamHaze((FragPos + drift * 2.0) * 0.62) * 0.6
                        + beamHaze((FragPos - drift * 1.3) * 1.6) * 0.4;
        float wobble = (edgeNoise - 0.5) * 0.30;

        float cardFade = smoothstep(0.0, 0.38, border + wobble * 0.55);

        vec3 toEye = normalize(viewPos.xyz - FragPos);
        float facing = abs(dot(normalize(Normal), toEye));
        float coneFade = mix(1.0, smoothstep(0.0, 0.62, facing + wobble), 0.8);

        // Three octaves of value noise on the world position: mist the beam
        // sweeps through, not a texture painted on the blade.
        //
        // Sampled in world space, and barely drifting. At a third of a yard a
        // second the drift and the sweep could not be told apart and the haze
        // read as surface detail; what is wanted here is standing air, so the
        // field is very nearly fixed and what moves is the beam. Coarse, too -
        // a cell is about ten yards, the scale of a bank of mist, where three
        // cells to a yard looked like noise on a surface.
        float haze = beamHaze((FragPos + drift) * 0.17) * 0.55
                   + beamHaze((FragPos - drift * 0.6) * 0.44) * 0.30
                   + beamHaze((FragPos + drift * 1.7) * 1.05) * 0.15;
        // Taken hard enough to punch holes, and never quite to nothing.
        //
        // A beam is several additive cards laid over each other and their
        // contributions sum, so the core is saturated white: dimming a layer
        // to a fifth still adds to white through the middle, which is why a
        // 0.55-to-1.15 multiply showed nothing at all. The troughs have to go
        // nearly black before the core breaks up - but not to zero, or the
        // beam flickers inside its own haze rather than swirling.
        haze = mix(0.26, 1.35, smoothstep(0.22, 0.78, haze));

        // And a fade along the beam's own length, which does not care how the
        // mesh was built or how its UVs run.
        //
        // The card fade above needs the UVs to reach 0 and 1 at the card's
        // border to find it, and the zeppelin's upper beam does not: its
        // triangle keeps a crisp outline while the lower beam softens. The
        // cone fade cannot help there either, because a flat card turned
        // toward the eye has no silhouette to catch. Distance from the
        // emitter is the one handle every beam has, whatever its geometry -
        // and a searchlight genuinely does give out as it reaches.
        float reach = length(FragPos - InstanceOrigin);
        float lengthFade = 1.0 - smoothstep(10.0, 52.0, reach) * 0.45;

        // Four attenuations multiplied together take a beam to nothing long
        // before any one of them looks wrong: at 0.7 each the product is a
        // quarter, and the searchlight all but disappeared. Each is gentler
        // now, and the whole is lifted so the middle of the beam comes back
        // to roughly the brightness it had while the edges still go.
        float beamFade = cardFade * coneFade * haze * lengthFade * 2.3;
        outAlpha *= beamFade;
        // Additive beams carry their brightness in the colour rather than the
        // alpha, so fading one means dimming it.
        if (blendMode >= 3) result *= beamFade;
    }

    // A flame stops before its card does.
    //
    // The bonfire's fire is drawn on cards whose material says opaque, so the
    // black backing around the flame came out as a rectangle - a hard-edged
    // slab with a straight top. Keying the black out and adding rather than
    // covering deals with the backing; this deals with the edge, which is
    // still an edge wherever the texture is bright right up to it.
    //
    // ModelHeight is how far up its own model this fragment sits, so the fade
    // lands in the same place whatever size the fire is placed at.
    if (fireCard != 0) {
        float tipFade = 1.0 - smoothstep(0.55, 0.98, ModelHeight);
        outAlpha *= tipFade;
        // An additive card carries its brightness in the colour, so fading one
        // means dimming it.
        if (blendMode >= 3) result *= tipFade;
    }

    // A doodad that has just streamed in, dissolving in. After every
    // texture read, so the pixels it drops take no derivative with them.
    if (streamFadeHidden(vStreamHide)) discard;
    outColor = vec4(result, outAlpha);
}
