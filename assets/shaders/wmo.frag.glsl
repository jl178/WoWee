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
};

#include "rt_lighting.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int unlit;
    int isInterior;
    float specularIntensity;
    int reserved0;
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
    int padding2;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;

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

float sampleShadowPCF(sampler2DShadow smap, vec3 coords) {
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
    vec3 sum = vec3(0.0);
    for (int i = 0; i < min(localLightMeta.x, 64); ++i) {
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
    vec4 clip = projection * view * vec4(worldPos, 1.0);
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
    float fogFactor = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
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

    if (enablePOM != 0 && heightMapVariance > 0.001 && lodFactor < 0.99) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor, pomScaleHere);
    }

    vec4 texColor = hasTexture != 0 ? textureGrad(uTexture, finalUV, uvDx, uvDy) : vec4(1.0);
    if (alphaTest != 0 && texColor.a < 0.5) discard;

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

    vec3 result;

    // Sample the sun shadow map. Interior groups ignore it below.
    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        float normalOffset = shadowTexel() * 2.0 * (1.0 - abs(dot(norm, ldir)));
        vec3 biasedPos = FragPos + norm * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 &&
            proj.y >= 0.0 && proj.y <= 1.0 &&
            proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(norm, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
        }
        shadow = mix(1.0, shadow, shadowParams.y);
    }
    RtLight rt = rtLightAt(FragPos);
    shadow = rtShadow(rt, shadow);

    if (emissive == 1) {
        // Authored luminous glass must remain bright in direct sun and shadow.
        // A small warm bias keeps low-valued texels from reading as dark glass.
        vec3 glass = texColor.rgb * 2.0 + vec3(0.16, 0.07, 0.015);

        // Gentle guttering, weaker than the clock's open fire - these are steady
        // lamps, not flames in the wind.
        //
        // Every lamp in a building shares one batch, so a uniform phase would
        // pulse a whole street in lockstep. The phase is hashed from the lamp's
        // world position instead, quantised into cells a few units across: large
        // enough that one lamp's glass falls in a single cell, small enough that
        // neighbouring lamps land in different ones.
        vec3 cell = floor(FragPos * 0.2);
        float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        float phase = h * 6.2831853;
        float t = fogParams.z;
        float flicker = 0.93
                      + 0.05 * sin(t * 1.3 + phase)
                      + 0.02 * sin(t * 2.9 + phase * 1.7);
        result = glass * flicker;
    } else if (emissive == 2) {
        // Firelit from behind (Darkshire's clock face): the surface is still lit
        // by the sun so it belongs to the building by day, with a warm glow
        // seeping through it as if a fire burned in the tower.
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);
        vec3 lit = texColor.rgb * (ambientColor.rgb + lightColor.rgb * diff * shadow);

        // Three detuned sines: a slow breathing sway, a quicker wobble, and a
        // faint fast jitter. Their periods share no common multiple over any
        // watchable span, so the flame never visibly loops.
        float t = fogParams.z;
        float flicker = 0.84
                      + 0.10 * sin(t * 1.3)
                      + 0.05 * sin(t * 2.9 + 1.7)
                      + 0.03 * sin(t * 6.7 + 0.6);

        // Firelight only competes with daylight once the sun is down, so fade the
        // glow up as the scene darkens. A small floor keeps it faintly visible in
        // daytime shade rather than switching on at dusk.
        float daylight = clamp(dot(ambientColor.rgb + lightColor.rgb,
                                   vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        float night = mix(1.0, 0.22, daylight);

        const vec3 kFireColor = vec3(1.0, 0.58, 0.22);
        result = lit + kFireColor * (0.30 * flicker * night);

        // Glass covering the dial: a tight sun highlight plus a Fresnel sheen
        // that picks up sky colour at grazing angles, which is what sells a pane
        // in front of the face rather than paint on stone. Both are additive and
        // unaffected by the fire, since they live on the outer surface.
        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float gloss  = pow(max(dot(norm, halfDir), 0.0), 96.0);
        float fresnel = pow(1.0 - clamp(dot(norm, viewDir), 0.0, 1.0), 4.0);
        result += lightColor.rgb * (gloss * 0.55 * shadow)
                + ambientColor.rgb * (fresnel * 0.35);
    } else if (isLava != 0) {
        // Lava is self-luminous - bright emissive, no shadows
        result = texColor.rgb * 1.5;
    } else if (isInterior != 0) {
        // WMO interior. The renderer has already run the client's
        // FixColorVertexAlpha over the vertex colours (interiorVertexLight in
        // wmo_renderer.cpp): rgb is the baked light at half scale, rebased off
        // the MOHD ambient, and alpha is how much of the outdoors reaches the
        // vertex - 0 in the room, rising across the trans batches at an
        // opening, 1 on the exterior batches.
        //
        // Indoors the light is the ambient plus the baked colour doubled,
        // which is the MOCV as authored. The sun's shadow map is not applied
        // to it: the roof always shadows the room. A small floor stays for the
        // roots that carry no ambient at all (Darnassus is one).
        vec3 wmoAmbient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
        vec3 baked = VertColor.rgb * 2.0;
        vec3 interiorLight = max(wmoAmbient + baked, vec3(0.12));
        vec3 light = interiorLight;
        // Toward an opening the room's light gives way to the outdoor light,
        // sun and shadow included, the way an exterior group is lit. The baked
        // colour has already been faded by the same alpha, so it goes to
        // nothing where the opening does.
        float outdoors = clamp(VertColor.a, 0.0, 1.0);
        if (outdoors > 0.0) {
            vec3 ldir = normalize(-lightDir.xyz);
            float diff = max(dot(norm, ldir), 0.0);
            vec3 exteriorLight = rtAmbient(rt, ambientColor.rgb)
                               + shadow * diff * lightColor.rgb + baked;
            light = mix(interiorLight, exteriorLight, outdoors);
        }
        result = texColor.rgb * light;
    } else if (unlit != 0) {
        // F_UNLIT: the client does no lighting on these, so they are drawn
        // at the texture's own brightness, in sun or shade. Darnassus'
        // glowing amber screens (MM_DRNSS_SCREEN_01) on the sides of its
        // houses are unlit, and they stand under the canopy's shadow: taking
        // the shadow made them black squares.
        result = texColor.rgb;
    } else {
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);

        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;

        result = rtAmbient(rt, ambientColor.rgb) * texColor.rgb
               + shadow * (diff * lightColor.rgb * texColor.rgb + spec * lightColor.rgb);

        // An exterior group's vertex colours play no part in its light. The
        // client's FixColorVertexAlpha gives every vertex of an EXTERIOR group
        // (past its trans batches, and exterior groups have none) an alpha of
        // 255, and that alpha is the interior/exterior blend: at 255 the
        // surface takes the outdoor light alone, the same ambient and sun the
        // doodads and the terrain take. 296 of the 352 exterior groups in the
        // data that carry MOCV ship that alpha already.
        //
        // They were multiplied in once (Stormwind's two exterior groups with
        // MOCV, mean 8 of 255, drew near black), then added at half, which put
        // a lamp-pool glow on walls that the stock client draws plain.
    }

    // F_SIDN: windows and lamps glow at night in the material's sidnColor,
    // added to whatever light the surface already has. The client fades it
    // in by the time of day; that is not in this shader's inputs, so how dark
    // the outdoor light is stands in for it, as the clock face's fire does
    // above. An unlit material is at full brightness already.
    if (sidnColor != 0u && unlit == 0 && emissive == 0 && isLava == 0) {
        vec3 sidn = vec3(float((sidnColor >> 16) & 0xFFu),
                         float((sidnColor >> 8) & 0xFFu),
                         float(sidnColor & 0xFFu)) / 255.0;
        float daylight = clamp(dot(ambientColor.rgb + lightColor.rgb,
                                   vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        float night = 1.0 - smoothstep(0.25, 0.65, daylight);
        result += texColor.rgb * sidn * night;
    }

    if (isLava == 0)
        result += localLightContribution(FragPos, norm, texColor.rgb);

    float dist = length(viewPos.xyz - FragPos);
    result = applyFog(result, FragPos, dist);

    // Opaque and alpha-key materials write their texture's alpha unused;
    // blended ones (blend mode 2 and up) are drawn by the transparent
    // pipeline with it.
    float alpha = texColor.a;

    outColor = vec4(result, alpha);
}
