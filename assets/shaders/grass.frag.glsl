#version 450

// Root-to-tip gradient with enough directional light to read the curve.
//
// Full shading - terrain colour influence, upright-normal blend, subsurface -
// is Phase 6. What is here exists so the blade's shape is legible: a purely
// flat colour hides the taper and the bend, which are the things worth looking
// at while they are being tuned.

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
    // The rest of the block as far as the sun's shadow maps
    // (shadow_filter.glsli), for the reference client's sun shadows on the blades.
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
    vec4 detailShadowParams;
};

#include "shadow_filter.glsli"

// The stock depth fog every other world surface takes, so a blade fades
// into the haze exactly as the ground it stands on does.
#include "stock_fog.glsli"

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in float vHeightT;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vRootColor;
layout(location = 3) in vec3 vTipColor;
layout(location = 4) in vec4 vGroundColor;
layout(location = 5) in vec4 vHeadColor;
layout(location = 6) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

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
    // Colours come from the ground's own vegetation profile, so scree, dry
    // scrub and meadow differ without anything here knowing which zone it is.
    vec3 albedo = mix(vRootColor, vTipColor, vHeightT * vHeightT);

    // Pulled toward the terrain's own colour, mixed rather than replaced
    // (spec 8), and more strongly at the root than the tip - the root is what
    // sits against the ground, and an unmixed field reads as green paint over
    // brown earth. w is zero when no colour was available, which disables the
    // mix instead of pulling toward black.
    // Only the base needs seating against the earth now - the blade's whole
    // colour already came from the ground's own tones.
    float groundMix = vGroundColor.w * mix(0.35, 0.0, smoothstep(0.0, 0.45, vHeightT));
    albedo = mix(albedo, vGroundColor.rgb, groundMix);

    // Seed heads and blooms take over the top of the blade. After the ground
    // mix on purpose: a bloom should be the one thing in the field that does
    // not take the earth's colour.
    albedo = mix(albedo, vHeadColor.rgb,
                 vHeadColor.a * smoothstep(0.50, 0.72, vHeightT));

    // Two-sided: a blade is one strip and is lit from whichever face is turned
    // to the camera.
    vec3 n = normalize(vNormal) * (gl_FrontFacing ? 1.0 : -1.0);

    // Wrapped diffuse, so the unlit side falls off softly rather than going
    // black - grass is thin enough to pass light through.
    // Under enhanced daylight (bit 128) the blades take the ground's light,
    // as clutter is lit from the terrain under it (and as Wowee's M2 clutter
    // already is): with the light from the sun's disc, a high noon sun
    // otherwise left upright blades in their own half-light.
    if ((localLightMeta.z & 128) != 0) n = vec3(0.0, 0.0, 1.0);
    float ndl = dot(n, normalize(-lightDir.xyz));
    float wrapped = clamp(ndl * 0.5 + 0.5, 0.0, 1.0);

    // Roots sit in the shade of everything above them.
    float ao = mix(0.7, 1.0, vHeightT);

    // Light through the blade: looking toward the sun the upper blade glows
    // with the light behind it, as a meadow does against a low sun (WoW
    // the reference client's Goldshire afternoon). Without it the field read as dark
    // stubs exactly where the sun made everything else brightest. Strongest
    // looking straight into the sun, gone by 60 degrees off it, and only
    // while the sun is up. The sun is the sprite's direction and its day
    // weight (localLightMeta.w, leaf_light.hpp): the light direction stays up
    // all night and is not where the sun stands, and the way it travels
    // points away from the eye looking into the sun, which lit the blades
    // with the sun behind the camera instead.
    vec4 sun = unpackSnorm4x8(uint(localLightMeta.w));
    float facing = clamp(dot(normalize(vWorldPos - viewPos.xyz), sun.xyz), 0.0, 1.0);
    float through = facing * facing * facing * vHeightT * max(sun.w, 0.0);

    // The reference client's sun shadows (enh_bits::kEnhancedShadows, 512): the blade
    // takes the sun's shadow where it stands, one tap, as the reference client's clutter
    // does; without it a field under the trees glowed in full sun.
    // As deep as the ground under the blade takes it (terrain.frag): the
    // whole of the sun under the sky shade (256), else the map's share.
    float sunLit = 1.0;
    if ((localLightMeta.z & 512) != 0 && shadowParams.x > 0.5) {
        float s = sunShadowCheap(vWorldPos, normalize(-lightDir.xyz));
        sunLit = (localLightMeta.z & 256) != 0 ? s : mix(1.0, s, clamp(shadowParams.y, 0.0, 1.0));
    }

    vec3 lit = albedo * (ambientColor.rgb + lightColor.rgb * (wrapped + 0.9 * through) * sunLit) * ao;
    // Fogged as the ground under it is, or a field stands out of the haze
    // the terrain it grows from has gone into.
    lit = applyFog(lit, vWorldPos, length(viewPos.xyz - vWorldPos));
    outColor = vec4(lit, 1.0);
}
