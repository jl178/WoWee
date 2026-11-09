#version 450

// HD water (E34, the "HD water" setting; not stock). Drawn in place of
// water.frag while the row is on, with the same vertex shader, layout and
// blend. The stock surface stays the base: the LiquidType texture sequence
// through its stock texture matrix, Light.dbc's close and far colours and
// LightParams' shallow and deep alphas, the stock light on it. Over that:
//
//   - surface normals from two scrolled layers of the stock frame itself
//     (the gradient of its brightness), no texture of our own;
//   - colour and clarity from the real depth below the surface, read from
//     the scene's depth copy: the close colour and a clearer surface in the
//     shallows, the far colour and the deep alpha further down; the ocean
//     takes the ocean colours, rivers, lakes and the canals the river ones;
//   - a soft edge where the water meets the shore (the foam is still the
//     "Shoreline foam" row's, drawn here as water.frag draws it);
//   - a Fresnel reflection of Light.dbc's sky dome by the stock ring rule,
//     or of the planar sky pass where "Water reflections" is on;
//   - the sun's glint, from the stock light direction and colour;
//   - a gentle refraction of the bed through the normals.
//
// The refraction needs the scene's colour as it stood before the water
// (hdParams.y), which exists where the water draws after the scene pass.
// Then this shader composes the bed itself and writes alpha 1. Elsewhere
// (MSAA with the water in the scene pass) it blends as the stock water does,
// unbent.
//
// Magma and slime, water seen from below and water inside a building are the
// stock surface, unchanged.

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

#include "stock_fog.glsli"

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    mat4 model;
    float texScale;
    float texOffsetU;
    float texOffsetV;
    float liquidBasicType;
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float liquidTexValid;
    float shoreFoam;
    float depthRamp;
    float texRotCos;
    float texRotSin;
    float rainRipple;
} push;

#include "rain_ripples.glsli"

layout(set = 1, binding = 0) uniform WaterMaterial {
    vec4 vertexColor;
    float interior;
    float pad0;
    float pad1;
    float pad2;
};

layout(set = 2, binding = 0) uniform sampler2D SceneColor;
layout(set = 2, binding = 1) uniform sampler2D SceneDepth;
layout(set = 2, binding = 2) uniform sampler2D ReflectionColor;
const int MAX_WAKE_POINTS = 32;

layout(set = 2, binding = 3) uniform WaterFrameData {
    mat4 reflViewProj;
    mat4 historyViewProj;  // the camera SceneDepth/SceneColor were rendered with
    vec4 wakeBounds;
    vec4 wakePoints[MAX_WAKE_POINTS];
    vec4 riverClose;
    vec4 riverFar;
    vec4 oceanClose;
    vec4 oceanFar;
    // x: on; y: SceneColor holds this frame's scene before the water;
    // zw: the share of SceneColor that copy fills.
    vec4 hdParams;
    vec4 skyTop;
    vec4 skyMiddle;
    vec4 skyBand1;
    vec4 skyBand2;
    vec4 skySmog;
    vec4 skyFog;
};

layout(set = 3, binding = 0) uniform sampler2D LiquidTexture;

const float SHORE_FOAM_DEPTH = 0.5;
const float SHORE_FOAM_OPACITY = 0.45;
const float SHORE_FOAM_ALBEDO = 0.72;

// ---- HD constants ---------------------------------------------------------
// Yards of depth over which the colour runs from close to far (1 - 1/e of
// the way), and of path through the water over which it clouds over.
const float HD_COLOR_DEPTH_RIVER = 3.5;
const float HD_COLOR_DEPTH_OCEAN = 7.0;
const float HD_CLARITY_PATH_RIVER = 3.0;
const float HD_CLARITY_PATH_OCEAN = 5.0;
// The shallows' opacity as a share of LightParams' shallow alpha.
const float HD_SHALLOW_ALPHA = 0.4;
// The waterline: the surface fades in over this many yards of depth.
const float HD_SHORE_SOFT = 0.3;
// How strongly the stock frame's brightness bends the surface.
const float HD_NORMAL_STRENGTH = 0.35;
// Screen-space bend of the bed at full depth, as a share of the screen.
const float HD_REFRACTION = 0.022;
// The most the reflection takes at grazing angles.
const float HD_REFLECT_MAX = 0.4;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in float RampV;
layout(location = 4) in vec2 ScreenUV;

layout(location = 0) out vec4 outColor;

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

float cellularFoam(vec2 p, float jitter) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float minDist = 1.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 cellId = i + neighbor;
            vec2 point = vec2(hash21(cellId), hash22x(cellId)) * jitter
                       + vec2(0.5) * (1.0 - jitter);
            float d = length(neighbor + point - f);
            minDist = min(minDist, d);
        }
    }
    return minDist;
}
float cellularFoam(vec2 p) { return cellularFoam(p, 1.0); }

vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * (view * vec4(worldPos, 1.0));
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    float fogFactor = stockFogVisibility(worldPos);
    color = mix(fogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

// water.frag's depth below the surface (the same five taps), as the scene's
// linear depth behind this pixel, or -1 when nothing behind it is known.
float sceneLinBehind(vec2 screenUV, float waterLinDepth, float near, float far) {
    if (push.sceneValid < 0.5) return -1.0;
    vec2 texel = 1.0 / vec2(textureSize(SceneDepth, 0));
    const vec2 taps[5] = vec2[5](vec2(0.0), vec2(1.5, 0.0), vec2(-1.5, 0.0), vec2(0.0, 1.5), vec2(0.0, -1.5));
    for (int i = 0; i < 5; ++i) {
        vec2 uv = clamp(screenUV + taps[i] * texel, vec2(0.001), vec2(0.999));
        float sceneLin = linearizeDepth(texture(SceneDepth, uv).r, near, far);
        if (sceneLin >= waterLinDepth - 0.05) return max(sceneLin, waterLinDepth);
    }
    return -1.0;
}

// ---- Normals from the stock frame -------------------------------------------
// The frame's brightness (its rgb and its shimmer alpha) taken as a height.
float frameHeight(vec2 uv) {
    vec4 t = texture(LiquidTexture, uv);
    return dot(t.rgb, vec3(0.3333)) + 0.5 * t.a;
}

// The height's slope per yard on one layer, by forward differences a texel or
// a pixel's footprint apart, whichever is wider (so it does not shimmer far
// off). uv = world * scale.
vec2 frameSlope(vec2 uv, float scale) {
    float e = max(1.0 / float(textureSize(LiquidTexture, 0).x), length(fwidth(uv)));
    float h0 = frameHeight(uv);
    float hx = frameHeight(uv + vec2(e, 0.0));
    float hy = frameHeight(uv + vec2(0.0, e));
    return vec2(hx - h0, hy - h0) / e * scale;
}

// Two layers of the frame: one at the stock scale drifting one way, one a
// little over twice as large, turned and drifting another, so the two never
// line up into the stock tile.
vec3 hdSurfaceNormal(vec2 p, float time, float dist) {
    const float s1 = 0.06;
    const float s2 = 0.026;
    vec2 uv1 = p * s1 + vec2(0.017, 0.011) * time;
    vec2 q = vec2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8);
    vec2 uv2 = q * s2 + vec2(-0.009, 0.013) * time;
    vec2 slope = frameSlope(uv1, s1) * 0.6 + frameSlope(uv2, s2) * 0.9;
    float strength = HD_NORMAL_STRENGTH * mix(1.0, 0.35, smoothstep(40.0, 350.0, dist));
    return normalize(vec3(-slope * strength, 1.0));
}

// ---- The stock sky, for what the surface mirrors ------------------------------
// The skybox's rings (skybox.frag, 0x7f2470) without its dawn highlight: a
// dome lowered cos(pi/4) below the eye, colour linear in polar angle between
// rings at these fractions of pi.
vec3 stockSkyAt(vec3 dir) {
    // Written out ring by ring: a loop over local arrays of colours spills
    // to memory on the Iris Xe, and this runs on every reflecting pixel.
    const float h0 = 0.70710678;
    float dz = clamp(dir.z, -1.0, 1.0);
    float t = -h0 * dz + sqrt(max(h0 * h0 * dz * dz - h0 * h0 + 1.0, 0.0));
    float polar = acos(clamp(t * dz + h0, -1.0, 1.0)) / 3.14159265;
    if (polar <= 0.17) return mix(skyTop.rgb, skyMiddle.rgb, polar / 0.17);
    if (polar <= 0.20) return mix(skyMiddle.rgb, skyBand1.rgb, (polar - 0.17) / 0.03);
    if (polar <= 0.23) return mix(skyBand1.rgb, skyBand2.rgb, (polar - 0.20) / 0.03);
    if (polar <= 0.24) return mix(skyBand2.rgb, skySmog.rgb, (polar - 0.23) / 0.01);
    if (polar <= 0.25) return mix(skySmog.rgb, skyFog.rgb, (polar - 0.24) / 0.01);
    return skyFog.rgb;
}

// ---- Water (the reference client): its PBR water, cheaply (hdParams.x == 2) ---------
// docs/re/enhanced-water.md. Enhanced's deferred water shader (FDID 7552027)
// composes, per pixel, with t the depth of water behind the surface:
//   T      = exp(-t * density * absorb)                    (per channel)
//   body   = albedo * (1 - T) * (ambient + sun) light       (albedo between
//            LiquidType Color_1 and Color_2 by the wave's height)
//   out    = bed + sat(2t) * (body + reflection + sunSpec
//                             + (1 - Fsun) * mix(bed * T, foam, foamCover) - bed)
// with the reflection EnvBRDFApprox(roughness, N.V) (F0 0.02) times the
// environment, and foam from the depth behind the surface (shore and rocks),
// the distance to the shore and, in rivers, the flow. Constants per liquid
// from LiquidType 1.60.1: absorb = Float_2, Float_1, Float_0 as r, g, b (the
// columns run blue first, as the DB2's colours do: read that way every row's
// water takes the hue of its own colours), density = Float_3.
struct FwLiquid {
    vec3 absorb;    // per yard, before density, r g b
    float density;
    vec3 colHigh;   // Color_1 (wave crests)
    vec3 colLow;    // Color_2 (troughs)
    float foamBand; // yards of depth over which the shore/rock foam runs out
};

FwLiquid fwLiquidFor(bool isOcean, bool isCanal) {
    // 1250 "PBRWater - Generic - Ocean"
    if (isOcean) return FwLiquid(vec3(0.25, 0.75, 6.0).zyx, 8.0,
                                 vec3(0x48, 0xaa, 0xb0) / 255.0, vec3(0x33, 0x6d, 0x7c) / 255.0, 1.2);
    // 1344 "PBRWater - Stormwind - Canals"
    if (isCanal) return FwLiquid(vec3(0.3, 1.0, 1.0).zyx, 0.5,
                                 vec3(0x1b, 0xb2, 0xe6) / 255.0, vec3(0x23, 0x5b, 0x9a) / 255.0, 0.5);
    // 1324 "Generic - Lake - Forest" (Crystal Lake; 1240, 1288 and 1325 share
    // these columns)
    return FwLiquid(vec3(0.283, 0.477, 2.657).zyx, 2.0,
                    vec3(0x9b, 0xbf, 0xec) / 255.0, vec3(0x71, 0xb2, 0xdc) / 255.0, 0.7);
}

// The flow phase: two copies of the pattern carried down the river half a
// period apart, each faded out as it wraps (the usual two-phase flow map), so
// the offset stays bounded and no seam opens between triangles.
const float FW_FLOW_PERIOD = 2.4;  // seconds
const float FW_FLOW_SPEED = 1.4;   // yards a second at full flow

// Enhanced's surface is an FFT sea (LiquidTypeXFFTTile); here the stock
// frame's own ripples as HD water takes them, the fine layer carried by the
// flow and a calmer, rougher sheet than HD water's.
vec3 fwSurfaceNormal(vec2 p, vec2 flow, float time, float dist) {
    const float s1 = 0.06;
    const float s2 = 0.026;
    float ph = time / FW_FLOW_PERIOD;
    float fa = fract(ph), fb = fract(ph + 0.5);
    float wA = 1.0 - abs(1.0 - 2.0 * fa);
    vec2 drift = vec2(0.017, 0.011) * time;
    vec2 offA = flow * (fa * FW_FLOW_SPEED * FW_FLOW_PERIOD);
    vec2 offB = flow * (fb * FW_FLOW_SPEED * FW_FLOW_PERIOD) + vec2(0.37, 0.61) / s1;
    vec2 slopeA = frameSlope((p - offA) * s1 + drift, s1);
    vec2 slopeB = frameSlope((p - offB) * s1 + drift, s1);
    vec2 q = vec2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8);
    vec2 slope2 = frameSlope(q * s2 + vec2(-0.009, 0.013) * time, s2);
    vec2 slope = mix(slopeB, slopeA, wA) * 0.6 + slope2 * 0.9;
    // Flowing water is choppier (Enhanced's river foam and rougher crests).
    float strength = 0.3 * (1.0 + 0.8 * min(length(flow), 1.0))
                   * mix(1.0, 0.35, smoothstep(40.0, 350.0, dist));
    return normalize(vec3(-slope * strength, 1.0));
}

// Karis' EnvBRDFApprox, as Enhanced's water evaluates it (F0 0.02).
float fwEnvBrdf(float roughness, float NdotV) {
    vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) + vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2 ab = vec2(-1.04, 1.04) * a004 + r.zw;
    return 0.02 * ab.x + ab.y;
}

// Cheap cellular-ish foam pattern, 0..1.
float fwFoamPattern(vec2 p) {
    float n = noiseValue(p) * 0.55 + noiseValue(p * 2.7 + 17.0) * 0.3 + noiseValue(p * 6.1 - 5.0) * 0.15;
    return n;
}

void main() {
    float time = fogParams.z;
    float basicType = push.liquidBasicType;
    float dist = length(viewPos.xyz - FragPos);
    bool hasLiquidTex = push.liquidTexValid > 0.5;

    vec2 liquidUV;
    if (basicType > 1.5) {
        liquidUV = TexCoord + vec2(push.texOffsetU, push.texOffsetV);
    } else {
        liquidUV = vec2(TexCoord.x * push.texRotCos - TexCoord.y * push.texRotSin,
                        TexCoord.x * push.texRotSin + TexCoord.y * push.texRotCos) * push.texScale;
    }
    vec4 texel = texture(LiquidTexture, liquidUV);

    // Magma and slime: the stock surface (water.frag).
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

    // ---- The stock colour and alpha (water.frag, psLiquidWater) ----
    bool isOcean = push.depthRamp > 0.5 && push.depthRamp < 1.5;
    bool isWmoWater = push.depthRamp > 1.5;
    bool isInterior = interior > 0.5;
    vec4 closeCol = isOcean ? oceanClose : riverClose;
    vec4 farCol   = isOcean ? oceanFar   : riverFar;
    float depth01 = clamp(RampV, 0.0, 1.0);
    float rampT = clamp(depth01 * 64.0 - 0.5, 0.0, 63.0) / 64.0;
    vec4 rampCol = mix(closeCol, farCol, rampT);
    if (isWmoWater) rampCol.rgb = isInterior ? vec3(1.0) : riverFar.rgb;
    if (isOcean) rampCol.rgb *= mix(1.0, 0.9, clamp(depth01 * 64.0 - 62.5, 0.0, 1.0));

    vec3 surfaceLight = isInterior ? vec3(1.0)
                                   : clamp(ambientColor.rgb + lightColor.rgb * max(ldir.z, 0.0),
                                           vec3(0.0), vec3(1.0));
    vec3 color = rampCol.rgb * surfaceLight * vertexColor.rgb;
    float alpha = rampCol.a * vertexColor.a;
    if (hasLiquidTex) color += texel.rgb + vec3(0.25) * texel.a;

    // ---- HD ----
    // From above, outside: water seen from below and water in a building
    // keep the stock surface.
    bool hd = hdParams.x > 0.5 && !isInterior && viewPos.z > FragPos.z;
    bool composeBed = false;
    vec3 bedColor = vec3(0.0);
    vec3 fwBedT = vec3(1.0);  // Enhanced: the bed's share through the water
    // Outside the per-pixel branch, so its texture derivatives are sound
    // (hdParams is the same for every pixel).
    bool fw = hdParams.x > 1.5;
    // Enhanced: rivers flow. 3.3.5 has no flow data, but a river's sheet
    // slopes downhill; its gradient gives the flow's direction and speed.
    // (Derivatives here, in uniform control flow.)
    vec2 flowDir = vec2(0.0);
    float flowSpeed = 0.0;
    vec3 geoN = normalize(cross(dFdx(FragPos), dFdy(FragPos)));
    if (fw && abs(geoN.z) > 0.05) {
        vec2 downhill = geoN.xy / geoN.z * sign(geoN.z);
        float g = length(downhill);
        flowSpeed = smoothstep(0.004, 0.05, g);
        flowDir = g > 1e-5 ? downhill / g : vec2(0.0);
    }
    vec3 N = fw ? fwSurfaceNormal(FragPos.xy, flowDir * flowSpeed, time, dist)
                : (hdParams.x > 0.5 ? hdSurfaceNormal(FragPos.xy, time, dist) : vec3(0.0, 0.0, 1.0));
    vec2 screenUV = gl_FragCoord.xy / max(push.screenSize, vec2(1.0));
    float near = push.depthRange.x;
    float far = push.depthRange.y;
    float shore = 1.0;
    // The scene behind this pixel, read once for the HD surface and the foam.
    float waterLin = linearizeDepth(gl_FragCoord.z, near, far);
    float sceneLin = -1.0;
    if (hd || push.shoreFoam > 0.5) {
        // Read in the copy's camera, then carried onto this pixel's ray by
        // the ratio of the bed's depth to the surface's (historyLookup).
        vec2 historyUV;
        float historyLin;
        historyLookup(FragPos, screenUV, historyUV, historyLin);
        float behind = sceneLinBehind(historyUV, historyLin, near, far);
        if (behind >= 0.0) sceneLin = waterLin * (behind / max(historyLin, 1e-4));
    }
    if (hd) {

        // Depth below the surface along this ray: straight down for the
        // colour, the path through the water for the clarity.
        float vDepth, path;
        bool depthKnown = sceneLin > 0.0;
        if (depthKnown) {
            vec3 bed = viewPos.xyz + (FragPos - viewPos.xyz) * (sceneLin / max(waterLin, 1e-4));
            vDepth = max(FragPos.z - bed.z, 0.0);
            path = length(bed - FragPos);
        }

        if (fw) {
            // ---- Water (the reference client) ----
            FwLiquid L = fwLiquidFor(isOcean, isWmoWater);
            // Depth of water behind the surface, as Enhanced measures it: the
            // view depth of the bed past the surface's.
            float t = depthKnown ? max(sceneLin - waterLin, 0.0) : 60.0;
            composeBed = hdParams.y > 0.5 && push.sceneValid > 0.5;
            if (composeBed) {
                // Enhanced bends the bed by the surface's slope times the
                // depth (uv + n.xy * 0.005 * min(t, 50)); the same here, in
                // view space, eased off with distance.
                vec2 uvR = screenUV;
                if (depthKnown) {
                    vec3 vN = mat3(view) * vec3(N.xy, 0.0);
                    float k = 0.012 * min(t, 12.0) * (1.0 - smoothstep(30.0, 200.0, dist));
                    vec2 cand = clamp(screenUV + vN.xy * k, vec2(0.001), vec2(0.999));
                    float candLin = linearizeDepth(texture(SceneDepth, cand).r, near, far);
                    if (candLin >= waterLin) {
                        uvR = cand;
                        t = max(t, candLin - waterLin);
                    }
                }
                bedColor = texture(SceneColor, uvR * hdParams.zw).rgb;
            }
            // Absorption over the depth; the scene is display-encoded, so the
            // transmittance is taken to that encoding.
            vec3 T = pow(exp(-t * L.absorb * L.density), vec3(1.0 / 2.2));

            // The light: Enhanced's sun term is gated by the sun's height
            // (smoothstep of (sin elevation + 0.1) * 5) and enters through
            // the surface's Fresnel transmission, 0.98 (1 - (1 - cos)^5).
            // The sun sprite's own direction (the stock light stays up all
            // night): LightingParams::sunDir in the sky .w's.
            vec3 sunDir = normalize(vec3(skyBand1.w, skyBand2.w, skySmog.w) + vec3(0.0, 0.0, 1e-4));
            float sunUp = smoothstep(0.0, 1.0, clamp((sunDir.z + 0.1) * 5.0, 0.0, 1.0));
            float cosSun = clamp(sunDir.z, 0.0, 1.0);
            vec3 sunIn = lightColor.rgb * sunUp * 0.98 * (1.0 - pow(1.0 - cosSun, 5.0));
            vec3 amb = ambientColor.rgb;
            float shadowF = 1.0;
            if (shadowParams.x > 0.5 && sunUp > 0.01) {
                vec4 ls = lightSpaceMatrix * vec4(FragPos, 1.0);
                vec3 pr = ls.xyz / ls.w;
                pr.xy = pr.xy * 0.5 + 0.5;
                if (pr.x >= 0.0 && pr.x <= 1.0 && pr.y >= 0.0 && pr.y <= 1.0 && pr.z >= 0.0 && pr.z <= 1.0) {
                    shadowF = texture(uShadowMap, vec3(pr.xy, pr.z - 0.0015));
                }
            }

            // In-scattering: the albedo between Color_2 (troughs) and
            // Color_1 (crests) by the ripple's height, times (1 - T), lit by
            // the sky and the sun, forward-scattered toward a low sun (HG).
            float hgt = hasLiquidTex ? clamp(dot(texel.rgb, vec3(0.3333)) + 0.5 * texel.a - 0.15, 0.0, 1.0) : 0.5;
            vec3 albedo = mix(L.colLow, L.colHigh, hgt);
            float cosVL = dot(-viewDir, ldir);
            const float g = 0.6;
            float hg = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosVL, 1e-3), 1.5) * 0.25;
            // Scaled to the display-encoded scene: Enhanced's linear albedo
            // times its light comes out near 0.6 of the colour by day.
            vec3 inLight = amb * 0.55 + sunIn * shadowF * (0.12 + 0.12 * hg);
            vec3 bodyF = albedo * (vec3(1.0) - T) * inLight;

            // Reflection: EnvBRDFApprox at F0 0.02 of the environment -
            // Light.dbc's sky by the ring rule, or the planar sky pass.
            vec3 Nr = normalize(mix(vec3(0.0, 0.0, 1.0), N, 0.6));
            float NdotV = max(dot(Nr, viewDir), 1e-3);
            float F = fwEnvBrdf(0.06, NdotV);
            vec3 R = reflect(-viewDir, Nr);
            R.z = max(R.z, 0.02);
            vec3 env = stockSkyAt(normalize(R));
            vec4 reflClip = reflViewProj * vec4(FragPos, 1.0);
            if (reflClip.w > 0.1) {
                vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;
                reflUV.y = 1.0 - reflUV.y;
                reflUV += N.xy * 0.03;
                float edgeFade = smoothstep(0.0, 0.15, reflUV.x) * smoothstep(1.0, 0.85, reflUV.x)
                               * smoothstep(0.0, 0.15, reflUV.y) * smoothstep(1.0, 0.85, reflUV.y);
                vec3 sky = texture(ReflectionColor, clamp(reflUV, vec2(0.002), vec2(0.998))).rgb;
                float valid = smoothstep(0.002, 0.05, dot(sky, vec3(0.299, 0.587, 0.114))) * edgeFade;
                // The pass's sun and moon sprites mirror as hard discs cut
                // by its target's edge; the highlight below is the sun's
                // and moon's reflection here, so the pass gives only the sky.
                env = mix(env, min(sky, env * 1.6 + 0.05), valid);
            }

            // The sun's (by night the moon's) highlight: GGX on a calm
            // sheet, Fresnel at the half vector; in the HDR scene it may pass
            // 1 and bloom.
            // By day toward the sun sprite; by night toward the moon, taken
            // opposite the sun, dimmer, in the night light's colour.
            bool byDay = sunDir.z > -0.05;
            vec3 Ls = byDay ? sunDir : normalize(vec3(-sunDir.xy, max(-sunDir.z, 0.15)));
            vec3 H = normalize(Ls + viewDir);
            float NdotH = max(dot(N, H), 0.0);
            float VdotH = max(dot(viewDir, H), 0.0);
            float NdotL = max(dot(N, Ls), 0.0);
            const float a2 = 0.05 * 0.05;
            float dd = NdotH * NdotH * (a2 - 1.0) + 1.0;
            float D = a2 / (3.14159265 * dd * dd);
            float Fh = 0.02 + 0.98 * pow(1.0 - VdotH, 5.0);
            bool hdrLight = (localLightMeta.z & 32) != 0;
            float specF = min(D * Fh * NdotL / max(4.0 * NdotV, 0.4), hdrLight ? 6.0 : 1.2);
            vec3 specCol = byDay ? lightColor.rgb * sunUp * shadowF
                                 : lightColor.rgb * 0.35 * clamp(skyMiddle.w, 0.0, 1.0);
            vec3 sunSpec = specCol * specF;
            float Fsun = Fh * sunUp;

            // Foam: a band where the bed comes up to the surface - the shore
            // and every rock (Enhanced: smoothstep(1 - t / width)^4) - and, in
            // flowing water, foam carried down the river.
            float near01 = 1.0 - smoothstep(60.0, 180.0, dist);
            float cover = 0.0;
            float band = 0.0;
            if (depthKnown) {
                float x = clamp(1.0 - t / L.foamBand, 0.0, 1.0);
                band = pow(x * x * (3.0 - 2.0 * x), 4.0);
            }
            // The pattern only where there is foam to draw (most of a lake
            // has none).
            if (near01 > 0.0 && (band > 0.002 || flowSpeed > 0.0)) {
                float ph = time / FW_FLOW_PERIOD;
                float fa = fract(ph), fb = fract(ph + 0.5);
                float wA = 1.0 - abs(1.0 - 2.0 * fa);
                vec2 fl = flowDir * flowSpeed * FW_FLOW_SPEED * FW_FLOW_PERIOD;
                vec2 fp = FragPos.xy * 1.1 + vec2(time * 0.05, -time * 0.035);
                float pat = mix(fwFoamPattern(fp - fl * fb * 1.1 + 7.3),
                                fwFoamPattern(fp - fl * fa * 1.1), wA);
                float river = flowSpeed * smoothstep(0.42, 0.78, pat);
                cover = clamp(band * smoothstep(0.25, 0.6, pat) * 1.7 + river * 0.75, 0.0, 1.0) * near01;
            }
            float crestLit = sunUp * shadowF * clamp(dot(N, ldir) + 0.25, 0.0, 1.0);
            vec3 foamCol = mix(amb, clamp(lightColor.rgb, vec3(0.0), vec3(1.5)), crestLit) * 0.92;

            // Enhanced's composition: the bed through the water, then the
            // water's own light, faded in over the first half yard.
            float s2 = depthKnown ? clamp(t * 2.0, 0.0, 1.0) : 1.0;
            vec3 X = bodyF + env * F + sunSpec + (1.0 - Fsun) * foamCol * cover;
            vec3 Tb = T * (1.0 - Fsun) * (1.0 - cover);
            fwBedT = vec3(1.0 - s2) + s2 * Tb;
            color = s2 * X;
            shore = s2;
            if (composeBed) {
                alpha = 1.0;
            } else {
                alpha = clamp(1.0 - dot(fwBedT, vec3(1.0 / 3.0)), 0.0, 1.0);
                color = color / max(alpha, 0.004);
            }
            surfaceLight = amb + sunIn;
        } else {
        vec3 body;
        float op;
        // The canals' stock ramp is the river far colour alone, and so is
        // theirs here: only their clarity follows the depth.
        vec3 close = isWmoWater ? riverFar.rgb : closeCol.rgb;
        if (depthKnown) {
            float tCol = 1.0 - exp(-vDepth / (isOcean ? HD_COLOR_DEPTH_OCEAN : HD_COLOR_DEPTH_RIVER));
            body = mix(close, farCol.rgb, tCol);
            float tClear = 1.0 - exp(-path / (isOcean ? HD_CLARITY_PATH_OCEAN : HD_CLARITY_PATH_RIVER));
            op = mix(closeCol.a * HD_SHALLOW_ALPHA, farCol.a, tClear);
            shore = smoothstep(0.0, HD_SHORE_SOFT, vDepth);
        } else {
            body = rampCol.rgb;
            op = rampCol.a;
        }
        op *= vertexColor.a;

        vec3 lit = clamp(ambientColor.rgb + lightColor.rgb * max(dot(N, ldir), 0.0), vec3(0.0), vec3(1.0));
        vec3 bodyLit = body * lit * vertexColor.rgb;
        if (hasLiquidTex) bodyLit += texel.rgb + vec3(0.25) * texel.a;

        // Reflection, through half the surface's tilt: the full tilt sent
        // rays sideways into the canal walls and boats, dark blots on the
        // water.
        vec3 Nr = normalize(mix(vec3(0.0, 0.0, 1.0), N, 0.5));
        float NdotV = max(dot(Nr, viewDir), 0.001);
        float F = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
        F = min(F, HD_REFLECT_MAX) * shore;
        vec3 R = reflect(-viewDir, Nr);
        R.z = max(R.z, 0.02);
        R = normalize(R);
        vec3 refl = stockSkyAt(R);
        vec4 reflClip = reflViewProj * vec4(FragPos, 1.0);
        if (reflClip.w > 0.1) {
            vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;
            reflUV.y = 1.0 - reflUV.y;
            reflUV += N.xy * 0.03;
            float edgeFade = smoothstep(0.0, 0.15, reflUV.x) * smoothstep(1.0, 0.85, reflUV.x)
                           * smoothstep(0.0, 0.15, reflUV.y) * smoothstep(1.0, 0.85, reflUV.y);
            vec3 sky = texture(ReflectionColor, clamp(reflUV, vec2(0.002), vec2(0.998))).rgb;
            float valid = smoothstep(0.002, 0.05, dot(sky, vec3(0.299, 0.587, 0.114))) * edgeFade;
            refl = mix(refl, sky, valid);
        }
        // No screen-space reflection of the shore: five steps of it through
        // the depth copy cost 0.3 ms of a 1280x540 frame on the Iris Xe
        // where the water fills the view, four times that at 2560x1080
        // (enhancements-plan.md 3.19).
        composeBed = hdParams.y > 0.5 && push.sceneValid > 0.5;

        // The sun's glint: a tight highlight and a faint sheen round it, by
        // day, out of the shadow map's shade.
        vec3 H = normalize(ldir + viewDir);
        float NdotH = max(dot(N, H), 0.0);
        float spec = pow(NdotH, 400.0) * 2.2 + pow(NdotH, 48.0) * 0.06;
        spec *= smoothstep(-0.02, 0.15, ldir.z) * shore;
        if (spec > 0.004 && shadowParams.x > 0.5) {
            vec4 ls = lightSpaceMatrix * vec4(FragPos, 1.0);
            vec3 pr = ls.xyz / ls.w;
            pr.xy = pr.xy * 0.5 + 0.5;
            if (pr.x >= 0.0 && pr.x <= 1.0 && pr.y >= 0.0 && pr.y <= 1.0 && pr.z >= 0.0 && pr.z <= 1.0) {
                spec *= texture(uShadowMap, vec3(pr.xy, pr.z - 0.0015));
            }
        }
        vec3 glint = clamp(lightColor.rgb, vec3(0.0), vec3(1.0)) * spec;

        // What shows through, bent by the surface where there is depth for
        // it, unbent at the waterline so the shore meets it seamlessly. A
        // bent tap that lands on something in front of the water (a leg in
        // the shallows) falls back to the straight one.
        if (composeBed) {
            vec2 uvR = screenUV;
            if (depthKnown) {
                float k = HD_REFRACTION * smoothstep(0.0, 1.5, vDepth) * (1.0 - smoothstep(30.0, 200.0, dist));
                vec3 vN = mat3(view) * vec3(N.xy, 0.0);
                vec2 cand = clamp(screenUV + vN.xy * k, vec2(0.001), vec2(0.999));
                float candLin = linearizeDepth(texture(SceneDepth, cand).r, near, far);
                if (candLin >= waterLin) uvR = cand;
            }
            bedColor = texture(SceneColor, uvR * hdParams.zw).rgb;
        }

        // Out as the blend's (colour, alpha): bed * (1 - a) + colour * a =
        // bed * (1 - op)(1 - F) + body * op (1 - F) + reflection * F + glint.
        op *= shore;
        float a = 1.0 - (1.0 - op) * (1.0 - F);
        vec3 premul = bodyLit * op * (1.0 - F) + refl * F + glint;
        alpha = clamp(a + min(spec, 1.0) * (1.0 - a), 0.0, 1.0);
        color = min(premul / max(alpha, 0.004), vec3(8.0));
        surfaceLight = lit;
        }
    }

    // ---- Rain ripples (the "Rain ripples" setting), as water.frag ----
    if (push.rainRipple > 0.0) {
        float rw = rainRippleWidth(FragPos.xy);
        float nearFade = 1.0 - smoothstep(18.0, 32.0, dist);
        if (!isInterior && nearFade > 0.0) {
            vec2 slope = rainRippleSlope(FragPos.xy, time, push.rainRipple, rw);
            color += rainRippleLight(slope, FragPos, viewPos.xyz, surfaceLight * 0.45 + vec3(0.04)) * nearFade;
        }
    }

    // ---- The stock path's sky reflection ("Water reflections"), as
    // water.frag, where the HD one did not run ----
    if (!hd) {
        vec4 reflClip = reflViewProj * vec4(FragPos, 1.0);
        if (reflClip.w > 0.1) {
            vec3 norm = vec3(0.0, 0.0, 1.0);
            float NdotV = max(dot(norm, viewDir), 0.001);
            float fresnel = 0.02 + 0.98 * pow(1.0 - NdotV, 5.0);
            vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;
            reflUV.y = 1.0 - reflUV.y;
            float edgeFade = smoothstep(0.0, 0.15, reflUV.x) * smoothstep(1.0, 0.85, reflUV.x)
                           * smoothstep(0.0, 0.15, reflUV.y) * smoothstep(1.0, 0.85, reflUV.y);
            vec3 sky = texture(ReflectionColor, clamp(reflUV, vec2(0.002), vec2(0.998))).rgb;
            float valid = smoothstep(0.002, 0.05, dot(sky, vec3(0.299, 0.587, 0.114))) * edgeFade;
            color = mix(color, sky, clamp(fresnel * valid, 0.0, 0.35));
        }
    }

    // ---- Shoreline foam (the "Shoreline foam" setting), as water.frag ----
    if (push.shoreFoam > 0.5 && !(hd && fw)) {
        float depthBelow = sceneLin > 0.0 ? max(sceneLin - waterLin, 0.0) * max(abs(viewDir.z), 0.05) : -1.0;
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
                float cover = mix(smoothstep(0.30, 0.70, n), 1.0, band * 0.5);
                float shoreFoam = band * cover * (isOcean ? SHORE_FOAM_OPACITY : SHORE_FOAM_OPACITY * 0.7);
                // The HD surface fades out at the waterline; the foam keeps
                // its band a little way out from it rather than on the sand.
                if (hd) shoreFoam *= smoothstep(0.0, HD_SHORE_SOFT * 0.5, depthBelow);
                vec3 foamLight = ambientColor.rgb + lightColor.rgb * max(ldir.z, 0.0);
                vec3 foamColor = min(SHORE_FOAM_ALBEDO * foamLight, vec3(1.0));
                color = mix(color, foamColor, shoreFoam);
                alpha = max(alpha, shoreFoam);
            }
        }
    }

    // ---- Wake froth (the "Splashes and wakes" setting), as water.frag ----
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
    if (!composeBed && alpha < 1.0 / 255.0) discard;

    if (hd && fw && composeBed) {
        // The fog over the composed water as Enhanced lays it: the bed was
        // fogged with the scene, so only its share through the water is
        // kept and the fog added once (fogged(c) = c * s + o).
        float vis = stockFogVisibility(FragPos);
        vec3 s = vec3(vis);
        vec3 o = fogColor.rgb * (1.0 - vis);
        if (volumetricParams.x > 0.5) {
            vec4 air = fogVolumeAt(FragPos);
            s *= air.a;
            o = o * air.a + air.rgb;
        }
        outColor = vec4(max(bedColor - o, vec3(0.0)) * fwBedT + color * s + o, 1.0);
        return;
    }
    color = applyFog(color, FragPos, dist);
    if (composeBed) {
        outColor = vec4(mix(bedColor, color, alpha), 1.0);
    } else {
        outColor = vec4(color, alpha);
    }
}
