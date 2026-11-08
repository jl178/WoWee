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
};

#include "stock_fog.glsli"

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    mat4 model;
    float texScale;   // LiquidType Float[0] on water (0.25 for WMO ocean), else 1
    float texOffsetU; // magma/slime scroll this frame, LiquidType Float[0..1]
    float texOffsetV;
    float liquidBasicType;
    vec2 screenSize;  // size of the target being drawn into
    vec2 depthRange;  // the camera's own near and far, for linearising SceneDepth
    float sceneValid; // 1 once the depth capture holds a frame, 0 before it
    float liquidTexValid; // 1 when set 3 holds this liquid's texture frame
    float shoreFoam;      // 1 draws the shoreline foam band (not stock), 0 off
    float depthRamp;      // 0 river, 1 ocean, 2 WMO water (proceduralWmoWaterTex)
    float texRotCos;      // the stage 1 texture matrix's rotation (WMO Ocean's
    float texRotSin;      // stock 145.14 degrees; identity elsewhere)
    float rainRipple;     // rain ripples (not stock): how hard it rains, 0 off/dry
} push;

#include "rain_ripples.glsli"

// Per surface: the stock vertex colour (attrib 4) - white, or for a WMO group
// that is not exterior its liquid's MOMT colour - and whether that group's
// fixed interior light (every colour 1) lights it.
layout(set = 1, binding = 0) uniform WaterMaterial {
    vec4 vertexColor;
    float interior;
    float pad0;
    float pad1;
    float pad2;
};

// SceneColor is still bound but no longer read: the stock water does not
// refract. Only the depth copy is read, by the shoreline foam (a setting).
layout(set = 2, binding = 0) uniform sampler2D SceneColor;
layout(set = 2, binding = 1) uniform sampler2D SceneDepth;
layout(set = 2, binding = 2) uniform sampler2D ReflectionColor;
// Wake points are laid down behind anything wading through the shallows. Each
// is a churn site that spreads and dies, so a running character leaves a trail
// of froth rather than a disc stuck under its feet.
const int MAX_WAKE_POINTS = 32;

layout(set = 2, binding = 3) uniform WaterFrameData {
    mat4 reflViewProj;
    mat4 historyViewProj;  // the camera SceneDepth/SceneColor were rendered with
    vec4 wakeBounds;                  // xy = centre, z = cull radius, w = count
    vec4 wakePoints[MAX_WAKE_POINTS]; // xy = world pos, z = age 0..1, w = strength
    // Light.dbc liquid colours (LightIntBand 14-17), rgb, with LightParams'
    // shallow alpha in the close colour's a and deep alpha in the far one's.
    vec4 riverClose;
    vec4 riverFar;
    vec4 oceanClose;
    vec4 oceanFar;
};

// One frame of the liquid's LiquidType texture sequence (lake_a.%d.blp and
// the rest), picked by the renderer from the clock.
layout(set = 3, binding = 0) uniform sampler2D LiquidTexture;

// The shoreline foam: yards of depth the band reaches out to at most, how
// opaque it gets on the ocean (rivers and lakes take 70% of that), and how
// pale it is against the light falling on it.
const float SHORE_FOAM_DEPTH = 0.5;
const float SHORE_FOAM_OPACITY = 0.45;
const float SHORE_FOAM_ALBEDO = 0.72;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in float RampV;   // the depth ramp coordinate (stock attrib 7)
layout(location = 4) in vec2 ScreenUV;

layout(location = 0) out vec4 outColor;

// ============================================================
// Dual-scroll detail normals (multi-octave ripple overlay)
// ============================================================
float noiseValue(vec2 p);  // defined below; used to bend the wave fronts

vec3 dualScrollWaveNormal(vec2 p, float time) {
    // Three wave octaves at different angles, frequencies, and speeds.
    // Directions are non-axis-aligned to prevent visible tiling patterns.
    // Frequency increases and amplitude decreases per octave (standard
    // multi-octave noise layering for natural water appearance).
    vec2 d1 = normalize(vec2(0.86, 0.51));   // ~30° from +X
    vec2 d2 = normalize(vec2(-0.47, 0.88));  // ~118° (opposing cross-wave)
    vec2 d3 = normalize(vec2(0.32, -0.95));  // ~-71° (third axis for variety)
    float f1 = 0.19, f2 = 0.43, f3 = 0.72;  // spatial frequency (higher = tighter ripples)
    float s1 = 0.95, s2 = 1.73, s3 = 2.40;  // scroll speed (higher octaves move faster)
    float a1 = 0.22, a2 = 0.10, a3 = 0.05;  // amplitude (decreasing for natural falloff)

    vec2 p1 = p + d1 * (time * s1 * 4.0);
    vec2 p2 = p + d2 * (time * s2 * 4.0);
    vec2 p3 = p + d3 * (time * s3 * 4.0);

    // Bend the wave fronts. Each octave on its own is a pure cosine, which is a
    // set of infinitely long straight ridges - in perspective those read as
    // bright parallel lines running to the horizon, and the specular highlight
    // rides along each crest. Perturbing the phase with low-frequency noise
    // makes the crests wander, at the same wavelength, amplitude and speed. The
    // noise is much coarser than the waves, so it curves them rather than
    // roughening them.
    float warp = noiseValue(p * 0.018 + vec2(time * 0.015)) * 6.28318;
    float c1 = cos(dot(p1, d1) * f1 + warp);
    float c2 = cos(dot(p2, d2) * f2 + warp * 1.7);
    float c3 = cos(dot(p3, d3) * f3 + warp * 2.6);

    float dHx = c1 * d1.x * f1 * a1 + c2 * d2.x * f2 * a2 + c3 * d3.x * f3 * a3;
    float dHy = c1 * d1.y * f1 * a1 + c2 * d2.y * f2 * a2 + c3 * d3.y * f3 * a3;

    return normalize(vec3(-dHx, -dHy, 1.0));
}

// ============================================================
// Linearize depth
// ============================================================
float linearizeDepth(float d, float near, float far) {
    return near * far / (far - d * (far - near));
}

// Where a surface point lies in the scene copy (SceneDepth), and that copy's
// camera depth of it. With MSAA or an upscaler the copy is an earlier frame's
// (WaterFrameUBOData::historyViewProj), so reading it at this frame's pixel
// read the scene from where the camera had been: the shoreline band trailed
// a turning camera and settled when it stopped. The point is projected by
// both cameras and this pixel's uv moved by the difference, which needs no
// assumption about the target's y direction and is nothing when the copy is
// this frame's.
void historyLookup(vec3 worldPos, vec2 screenUV, out vec2 uv, out float linDepth) {
    vec4 cc = projection * (view * vec4(worldPos, 1.0));
    vec4 hc = historyViewProj * vec4(worldPos, 1.0);
    uv = screenUV;
    linDepth = cc.w;
    if (cc.w <= 1e-4 || hc.w <= 1e-4) return;
    vec2 ndcC = cc.xy / cc.w;
    vec2 ndcH = hc.xy / hc.w;
    float ySign = abs(screenUV.y - (ndcC.y * 0.5 + 0.5)) <= abs(screenUV.y - (0.5 - ndcC.y * 0.5)) ? 1.0 : -1.0;
    uv = screenUV + vec2(ndcH.x - ndcC.x, (ndcH.y - ndcC.y) * ySign) * 0.5;
    linDepth = hc.w;
}

// ============================================================
// Noise functions for foam
// ============================================================
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float hash22x(vec2 p) {
    return fract(sin(dot(p, vec2(269.5, 183.3))) * 43758.5453);
}

float noiseValue(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Voronoi-like cellular noise for foam particles
// jitter parameter controls how much cell points deviate from grid centers
// (0.0 = regular grid, 1.0 = fully random within cell)
float cellularFoam(vec2 p, float jitter) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float minDist = 1.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 cellId = i + neighbor;
            // Jittered cell point - higher jitter = more irregular placement
            vec2 point = vec2(hash21(cellId), hash22x(cellId)) * jitter
                       + vec2(0.5) * (1.0 - jitter);
            float d = length(neighbor + point - f);
            minDist = min(minDist, d);
        }
    }
    return minDist;
}
float cellularFoam(vec2 p) { return cellularFoam(p, 1.0); }

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

// The water's depth below this pixel, in yards straight down, or -1 when it
// cannot be known.
//
// The depth copy is half resolution and holds everything drawn before the
// water, the player included. Where a character stands in front of the water,
// the taps along its outline read the character's own depth, nearer than the
// surface. Taken as they were, that read as zero depth: the shallow colour and
// alpha, wobbling with the surface, traced the character's outline over the
// water. A sample nearer than the surface is not a bed under the water, so it
// is not used; the neighbours are tried instead, and only when none of them
// lies behind the surface is the depth unknown.
float waterDepthBelow(vec2 screenUV, float waterLinDepth, float near, float far, float verticalFactor) {
    if (push.sceneValid < 0.5) return -1.0;
    vec2 texel = 1.0 / vec2(textureSize(SceneDepth, 0));
    const vec2 taps[5] = vec2[5](vec2(0.0), vec2(1.5, 0.0), vec2(-1.5, 0.0), vec2(0.0, 1.5), vec2(0.0, -1.5));
    for (int i = 0; i < 5; ++i) {
        vec2 uv = clamp(screenUV + taps[i] * texel, vec2(0.001), vec2(0.999));
        float sceneLin = linearizeDepth(texture(SceneDepth, uv).r, near, far);
        // A small allowance for the depth copy's precision and the surface's
        // own ripple: a bed within a few centimetres of the surface is the
        // waterline, not an occluder.
        if (sceneLin >= waterLinDepth - 0.05) {
            return max(sceneLin - waterLinDepth, 0.0) * max(verticalFactor, 0.05);
        }
    }
    return -1.0;
}

void main() {
    float time = fogParams.z;
    float basicType = push.liquidBasicType;
    float dist = length(viewPos.xyz - FragPos);
    bool hasLiquidTex = push.liquidTexValid > 0.5;

    // The liquid texture through its stock texture matrix. TexCoord is the
    // vertex's uv: world x,y x 0.06 on ADT liquid, the 25/6-yard cell grid on
    // WMO liquid, the data's own uv on magma and slime. Water scales and
    // rotates it by LiquidType Float[0] and Float[1] (0x8a5590); magma and
    // slime scroll it by Float[0..1] repeats a second (0x8a6090).
    vec2 liquidUV;
    if (basicType > 1.5) {
        liquidUV = TexCoord + vec2(push.texOffsetU, push.texOffsetV);
    } else {
        liquidUV = vec2(TexCoord.x * push.texRotCos - TexCoord.y * push.texRotSin,
                        TexCoord.x * push.texRotSin + TexCoord.y * push.texRotCos) * push.texScale;
    }
    vec4 texel = texture(LiquidTexture, liquidUV);

    // ============================================================
    // Magma / slime: psLiquidMagma, vertex colour x texture, opaque, unlit
    // ============================================================
    if (basicType > 1.5) {
        vec3 color = hasLiquidTex ? vertexColor.rgb * texel.rgb
                                  : vertexColor.rgb * (basicType < 2.5 ? vec3(0.9, 0.35, 0.05)
                                                                       : vec3(0.25, 0.6, 0.1));
        color = applyFog(color, FragPos, dist);
        outColor = vec4(color, 1.0);
        return;
    }

    vec3 viewDir = normalize(viewPos.xyz - FragPos);
    vec3 ldir = normalize(-lightDir.xyz);

    // ============================================================
    // Stock colour and alpha (psLiquidWater, 0x8a5590)
    // ============================================================
    //   primary = clamp(ambient + diffuse * N.L) * vertexColour
    //   rgb = primary * ramp.rgb + tex.rgb + (specular + 0.25) * tex.a
    //   a   = primary.a * ramp.a
    // ramp is the client's 8x64 depth texture, read at the vertex's depth
    // byte over 42 (river) or 255 (ocean, WMO): Light.dbc's close colour and
    // LightParams' shallow alpha at row 0 stepping toward the far colour and
    // deep alpha, 1/64 per row (0x8a2bf0); the ocean's last row a tenth
    // darker. WMO water's ramp is the river far colour outside and white
    // inside, only the alpha running shallow to deep (0x8a2ac0).
    bool isOcean = push.depthRamp > 0.5 && push.depthRamp < 1.5;
    bool isWmoWater = push.depthRamp > 1.5;
    bool isInterior = interior > 0.5;
    vec4 closeCol = isOcean ? oceanClose : riverClose;   // a = shallow alpha
    vec4 farCol   = isOcean ? oceanFar   : riverFar;     // a = deep alpha
    float depth01 = clamp(RampV, 0.0, 1.0);
    // Where a bilinear lookup of the 64-row texture lands: row 0 in the
    // shallowest half row, row 63 (63/64 of the way) in the deepest.
    float rampT = clamp(depth01 * 64.0 - 0.5, 0.0, 63.0) / 64.0;
    vec4 rampCol = mix(closeCol, farCol, rampT);
    if (isWmoWater) rampCol.rgb = isInterior ? vec3(1.0) : riverFar.rgb;
    if (isOcean) rampCol.rgb *= mix(1.0, 0.9, clamp(depth01 * 64.0 - 62.5, 0.0, 1.0));

    // The surface's light: the vertex colour register clamps the sum to 0..1
    // before it multiplies the ramp. The normal is straight up. A WMO group
    // that is not exterior has a fixed light of 1 (0x7d4f40).
    vec3 surfaceLight = isInterior ? vec3(1.0)
                                   : clamp(ambientColor.rgb + lightColor.rgb * max(ldir.z, 0.0),
                                           vec3(0.0), vec3(1.0));
    vec3 color = rampCol.rgb * surfaceLight * vertexColor.rgb;
    float alpha = rampCol.a * vertexColor.a;
    if (hasLiquidTex) {
        // The texture adds its ripples, and its alpha the shimmer: the
        // specular colour is the local lights' alone (0x834f60), so under the
        // sun only the constant 0.25 is left.
        color += texel.rgb + vec3(0.25) * texel.a;
    }

    // ============================================================
    // Rain ripples - the "Rain ripples" setting (not stock)
    // ============================================================
    // Rings where the drops land, out to about 30 yards (past that they
    // would only shimmer), never on water inside a building. 0 while it is
    // dry or with the setting off, and the block is skipped.
    if (push.rainRipple > 0.0) {
        float rw = rainRippleWidth(FragPos.xy);
        float near = 1.0 - smoothstep(18.0, 32.0, dist);
        if (!isInterior && near > 0.0) {
            vec2 slope = rainRippleSlope(FragPos.xy, time, push.rainRipple, rw);
            color += rainRippleLight(slope, FragPos, viewPos.xyz, surfaceLight * 0.45 + vec3(0.04)) * near;
        }
    }

    // ============================================================
    // Sky reflection - the "Water reflections" setting (not stock)
    // ============================================================
    // psLiquidWater has no reflection term; nothing in the stock client
    // mirrors the sky or the world (docs/re/water.md). The renderer zeroes
    // the matrix whenever the pass did not run, so this is skipped then.
    vec4 reflClip = reflViewProj * vec4(FragPos, 1.0);
    if (reflClip.w > 0.1) {
        vec3 norm = vec3(0.0, 0.0, 1.0);
        float detailFade = 1.0 - smoothstep(250.0, 1400.0, dist);
        if (detailFade > 0.0) {
            norm = normalize(mix(norm, dualScrollWaveNormal(FragPos.xy, time), 0.45 * detailFade));
        }
        float NdotV = max(dot(norm, viewDir), 0.001);
        const float F0 = 0.02;
        float fresnel = F0 + (1.0 - F0) * pow(1.0 - NdotV, 5.0);
        vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;
        reflUV.y = 1.0 - reflUV.y;
        reflUV += norm.xy * 0.015;
        float edgeFade = smoothstep(0.0, 0.15, reflUV.x) * smoothstep(1.0, 0.85, reflUV.x)
                       * smoothstep(0.0, 0.15, reflUV.y) * smoothstep(1.0, 0.85, reflUV.y);
        vec3 sky = texture(ReflectionColor, clamp(reflUV, vec2(0.002), vec2(0.998))).rgb;
        float valid = smoothstep(0.002, 0.05, dot(sky, vec3(0.299, 0.587, 0.114))) * edgeFade;
        color = mix(color, sky, clamp(fresnel * valid, 0.0, 0.35));
    }

    // ============================================================
    // Shoreline foam - the "Shoreline foam" setting (not stock)
    // ============================================================
    // A thin band where the water runs out over the bed, from the scene's
    // depth below the surface (the copy is only taken while this is on), so
    // it follows every beach, bank, rock and pier post. The band's reach
    // breathes slowly, out of step along the shore, and a scrolled noise
    // breaks it into patches. Lit by the sky's ambient and sun as a flat
    // pale surface would be, so it stays a pale edge at dusk and a dim one at
    // night rather than a white line.
    if (push.shoreFoam > 0.5) {
        vec2 screenUV = gl_FragCoord.xy / max(push.screenSize, vec2(1.0));
        float near = push.depthRange.x;
        float far = push.depthRange.y;
        float waterLinDepth = linearizeDepth(gl_FragCoord.z, near, far);
        vec2 historyUV;
        float historyLin;
        historyLookup(FragPos, screenUV, historyUV, historyLin);
        float depthBelow = waterDepthBelow(historyUV, historyLin, near, far, abs(viewDir.z));
        if (depthBelow >= 0.0) {
            float near01 = 1.0 - smoothstep(40.0, 150.0, dist);
            float phase = noiseValue(FragPos.xy * 0.07) * 6.28318;
            float reach = SHORE_FOAM_DEPTH * (0.75 + 0.25 * sin(time * 0.8 + phase));
            float band = 1.0 - smoothstep(0.0, reach, depthBelow);
            band *= band * near01;
            if (band > 0.002) {
                vec2 drift = vec2(time * 0.09, -time * 0.06);
                float n = noiseValue(FragPos.xy * 0.9 + drift) * 0.6
                        + noiseValue(FragPos.xy * 2.3 - drift * 1.7) * 0.4;
                // Solid right at the waterline, broken up a little way out.
                float cover = mix(smoothstep(0.30, 0.70, n), 1.0, band * 0.5);
                float shoreFoam = band * cover * (isOcean ? SHORE_FOAM_OPACITY : SHORE_FOAM_OPACITY * 0.7);
                vec3 foamLight = ambientColor.rgb + lightColor.rgb * max(ldir.z, 0.0);
                vec3 foamColor = min(SHORE_FOAM_ALBEDO * foamLight, vec3(1.0));
                color = mix(color, foamColor, shoreFoam);
                alpha = max(alpha, shoreFoam);
            }
        }
    }

    // ============================================================
    // Wake froth - the "Splashes, wakes and bubbles" setting (not stock)
    // ============================================================
    // No points are uploaded while it is off.
    float wakeFoam = 0.0;
    if (wakeBounds.w > 0.5) {
        vec2 toWake = FragPos.xy - wakeBounds.xy;
        if (dot(toWake, toWake) < wakeBounds.z * wakeBounds.z) {
            int wakeCount = clamp(int(wakeBounds.w), 0, MAX_WAKE_POINTS);
            for (int i = 0; i < wakeCount; ++i) {
                vec4 wp = wakePoints[i];
                float radius = mix(0.40, 1.45, wp.z);
                float d = length(FragPos.xy - wp.xy);
                float disc = 1.0 - smoothstep(0.0, radius, d);
                wakeFoam += disc * disc * wp.w * (1.0 - wp.z);
            }
            wakeFoam = clamp(wakeFoam, 0.0, 1.0);
            if (wakeFoam > 0.002) {
                vec2 churnWarp = vec2(
                    noiseValue(FragPos.xy * 3.4 + time * 0.5) - 0.5,
                    noiseValue(FragPos.xy * 3.4 + vec2(23.0) - time * 0.44) - 0.5
                ) * 0.9;
                vec2 churnUV = FragPos.xy + churnWarp;
                float cells = cellularFoam(churnUV * 12.0 + time * vec2(0.25, -0.18));
                float cover = 1.0 - smoothstep(0.08, 0.38, cells);
                float fine = cellularFoam(churnUV * 29.0 - time * vec2(0.14, 0.30));
                cover = max(cover * 0.85, (1.0 - smoothstep(0.05, 0.22, fine)) * 0.5);
                cover *= 0.55 + 0.45 * noiseValue(churnUV * 2.2 + time * 0.35);
                wakeFoam = min(wakeFoam * cover, 0.60);
                color = mix(color, vec3(0.86, 0.91, 0.95), wakeFoam);
            }
        }
    }
    alpha = max(alpha, wakeFoam * 0.55);

    alpha = clamp(alpha, 0.0, 1.0);
    // The stock alpha test passes alpha >= 1/255 (state 7 at 0x8a5590).
    if (alpha < 1.0 / 255.0) discard;
    // No fade toward the horizon: the stock water simply goes into the fog.

    color = applyFog(color, FragPos, dist);
    outColor = vec4(color, alpha);
}
