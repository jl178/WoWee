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

// The Enhanced path's value noise: the same shape as noiseValue on an
// arithmetic hash (no sin per corner), as it runs on every water pixel.
float fwHash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float fwNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fwHash(i);
    float b = fwHash(i + vec2(1.0, 0.0));
    float c = fwHash(i + vec2(0.0, 1.0));
    float d = fwHash(i + vec2(1.0, 1.0));
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
// from LiquidType 1.60.1: absorb = Float_0..2 as r, g, b (Enhanced's own
// Crystal Lake capture: the bed goes ochre, then olive, with depth - blue is
// lost first), density = Float_3.
struct FwLiquid {
    vec3 absorb;    // per yard, before density, r g b
    float density;
    vec3 colHigh;   // Color_1 (wave crests)
    vec3 colLow;    // Color_2 (troughs)
    float foamBand; // yards of depth over which the shore/rock foam runs out
};

FwLiquid fwLiquidFor(bool isOcean, bool isCanal) {
    // 1250 "PBRWater - Generic - Ocean"
    if (isOcean) return FwLiquid(vec3(0.25, 0.75, 6.0), 8.0,
                                 vec3(0x48, 0xaa, 0xb0) / 255.0, vec3(0x33, 0x6d, 0x7c) / 255.0, 1.2);
    // 1344 "PBRWater - Stormwind - Canals"
    if (isCanal) return FwLiquid(vec3(0.3, 1.0, 1.0), 0.5,
                                 vec3(0x1b, 0xb2, 0xe6) / 255.0, vec3(0x23, 0x5b, 0x9a) / 255.0, 0.5);
    // 1324 "Generic - Lake - Forest" (Crystal Lake; 1240, 1288 and 1325 share
    // these columns)
    return FwLiquid(vec3(0.283, 0.477, 2.657), 2.0,
                    vec3(0x9b, 0xbf, 0xec) / 255.0, vec3(0x71, 0xb2, 0xdc) / 255.0, 0.7);
}

// The flow phase: two copies of the pattern carried down the river half a
// period apart, each faded out as it wraps (the usual two-phase flow map), so
// the offset stays bounded and no seam opens between triangles.
const float FW_FLOW_PERIOD = 2.4;  // seconds
const float FW_FLOW_SPEED = 1.4;   // yards a second at full flow
// The lakes' waves, set against Enhanced's Crystal Lake (docs/re/enhanced-water.md
// 4): the band (yards), the slope of the longest wave, Phillips' L
// (wind^2 / g, yards), how much a wave across the wind keeps, and the
// fall-off of the short waves' slope, (lambda / lMax)^FW_TILT (the damping
// of the small waves that keeps an FFT sea's short ripples faint).
const float FW_LAKE_LMIN = 0.35;
const float FW_LAKE_LMAX = 8.0;
const float FW_LAKE_STEEP = 0.065;
const float FW_LAKE_WINDL = 0.5;
const float FW_CROSS = 0.8;
const float FW_TILT = 0.5;
// The bend of the crests: cycles of phase across the warp noise (9-yard).
const float FW_BEND = 2.0;

// Enhanced's surface is an FFT sea: per liquid, up to four FFTTile patches
// (LiquidTypeXFFTTile; FFTTile columns 0 = patch size in yards, 2 = wind
// speed). The lakes (1240/1288/1324/1325) take 16, 32 and 64-yard patches at
// winds of 3-4, the sea (1250) 128-512 yards at 2-6, the canals (1344) one
// 46-yard patch at 6. Its liquids have no ripple texture at all
// (LiquidTypeXTexture lists only the six foam maps): every ripple is the
// FFT's. A Phillips spectrum, A exp(-1 / (k L)^2) / k^4 with L = wind^2 / g,
// carries the same slope in every octave above its peak and almost none
// below it; the peak sits at waves of 2 pi L (about 8 yards at wind 3.5).
// Here sixteen directional waves stand in for it: wavelengths spaced
// geometrically over each class's band, each weighted by the root of that
// cut-off and tilted toward the long waves (FW_TILT), spread all round by
// the golden angle and weaker across the wind (the spectrum's |k.w|^2),
// deep-water dispersion, the crests bent by a broad noise so they break up.
// A wave shorter than about two pixels is faded out and its slope variance
// kept (subVar), which Enhanced's prepass writes beside the slopes and its
// shading turns into roughness: far water gets a wider, softer sun path
// instead of sparkling noise.
// Returns the slope (xy), the summed height (z) and the amplitude sum (w).
const int FW_WAVES = 12;
const int FW_WAVES_CANAL = 8;
const float FW_TWO_PI = 6.2831853;

vec4 fwWaveSum(vec2 p, vec2 bend, float time, vec2 dpx, vec2 dpy, float waveClass, out float subVar) {
    // Per class: shortest and longest wavelength (yards), the slope of one
    // wave above the spectrum's peak, and Phillips' L = wind^2 / g (yards).
    float lMin = FW_LAKE_LMIN, lMax = FW_LAKE_LMAX, steep = FW_LAKE_STEEP, windL = FW_LAKE_WINDL;  // lakes, rivers: wind 3-4
    if (waveClass > 0.5 && waveClass < 1.5) { lMin = 1.5; lMax = 64.0; steep = 0.07; windL = 2.6; }  // sea: winds 2-6
    int waves = FW_WAVES;
    // The canals: wind 6 over one 46-yard tile; narrow water seen close, at
    // a grazing angle almost everywhere - fewer, sparser waves.
    if (waveClass > 1.5) { lMin = 0.7; lMax = 23.0; steep = 0.06; windL = 3.7; waves = FW_WAVES_CANAL; }
    vec2 slope = vec2(0.0);
    float h = 0.0;
    float wsum = 0.0;
    subVar = 0.0;
    // Wavelengths spaced geometrically, longest first, stepped by one ratio
    // (so the loop needs no pow): the wavelength, the frequency (deep
    // water, sqrt(g k)), the short-wave tilt and Phillips' exponent
    // (-0.5 / (k L)^2, which goes as lambda^2) all scale per step.
    float ratio = pow(lMin / lMax, 1.0 / float(waves - 1));
    float lambda = lMax;
    float k = FW_TWO_PI / lMax;
    float kStep = 1.0 / ratio;
    float w = 0.8 * sqrt(9.8 * FW_TWO_PI / lMax);
    float wStep = inversesqrt(ratio);
    float tilt = 1.0;
    float tiltStep = pow(ratio, FW_TILT);
    float cutExp = -0.5 * (lMax * lMax) / (FW_TWO_PI * FW_TWO_PI * windL * windL);
    float cutStep = ratio * ratio;
    // Directions turned by the golden angle from one wave to the next (all
    // round the circle, never two alike; a rotation per step, no trig),
    // weaker across the wind; phases stepped the same way.
    const vec2 wind = vec2(0.8, 0.6);
    const float GA_C = -0.7373688, GA_S = 0.6754903;  // cos, sin of 2.39996
    vec2 dir = vec2(0.6, -0.8);
    float phase0 = 4.461;
    // The narrowest any wave's footprint can be, whatever its direction
    // (|dpx.d| + |dpy.d| is least where d is square to one of them): once
    // the waves are shorter than twice that, every one left is filtered out
    // and only its slope variance remains, summed in closed form (the tilt
    // falls geometrically, the cut-off is ~1 for short waves). Far water
    // stops after a few waves.
    float fpMin = abs(dpx.x * dpy.y - dpx.y * dpy.x) / max(max(length(dpx), length(dpy)), 1e-6);
    const float CROSS_MEAN2 = 0.75;  // mean of the spread weight squared
    for (int i = 0; i < waves; ++i) {
        if (fpMin * 2.0 >= lambda) {
            float q = tiltStep * tiltStep;
            float left = float(waves - i);
            subVar += 0.5 * steep * steep * tilt * tilt * CROSS_MEAN2 * (1.0 - pow(q, left)) / max(1.0 - q, 1e-4);
            break;
        }
        float amp = steep * exp(cutExp) * tilt * (FW_CROSS + (1.0 - FW_CROSS) * abs(dot(dir, wind)));
        // A wave smaller than about two pixels is only noise: its slope goes
        // to the roughness instead. Measured along its own direction (how
        // far its phase runs across a pixel), so a wave seen side-on at a
        // grazing angle stays while one running toward the eye is filtered.
        float footprint = abs(dot(dpx, dir)) + abs(dot(dpy, dir));
        float aa = 1.0 - smoothstep(0.5, 1.0, footprint * 2.0 / lambda);
        subVar += 0.5 * amp * amp * (1.0 - aa * aa);
        amp *= aa;
        // Each wave's phase bent by a broad noise field, a different way per
        // direction, so its crests break up after a few wavelengths as an
        // FFT sea's do instead of running on as straight lines.
        float arg = k * dot(dir, p) - w * time + phase0 + FW_TWO_PI * FW_BEND * dot(dir, bend);
        slope += dir * (amp * cos(arg));
        // The height (for the colour) is the long waves': a short wave's
        // height is amp / k, a few hundredths of the longest's.
        if (i < 5) {
            h += amp / k * sin(arg);
            wsum += amp / k;
        }
        lambda *= ratio;
        k *= kStep;
        w *= wStep;
        tilt *= tiltStep;
        cutExp *= cutStep;
        dir = vec2(dir.x * GA_C - dir.y * GA_S, dir.x * GA_S + dir.y * GA_C);
        phase0 += 2.3995;
    }
    return vec4(slope, h, wsum);
}

// The surface's normal (xyz) and its height as -1..1 of the waves' reach (w);
// the slope variance below the pixel in subVar.
vec4 fwSurfaceNormal(vec2 p, vec2 flow, float time, float dist, float waveClass, out float subVar) {
    vec2 dpx = dFdx(p), dpy = dFdy(p);
    // A gentle warp so the crests are not straight lines.
    vec2 warp = vec2(fwNoise(p * 0.11 + 3.1), fwNoise(p * 0.11 - 7.7)) - 0.5;
    vec2 q = p + warp * 2.4;
    vec2 bend = warp;  // the same noise bends each wave's phase (no second pair)
    vec4 wv;
    if (dot(flow, flow) > 1e-6) {
        // Rivers: the pattern carried down the slope, two phases half a
        // period apart, each faded out as it wraps (bounded offsets).
        float ph = time / FW_FLOW_PERIOD;
        float fa = fract(ph), fb = fract(ph + 0.5);
        float wA = 1.0 - abs(1.0 - 2.0 * fa);
        float vA, vB;
        vec4 a = fwWaveSum(q - flow * (fa * FW_FLOW_SPEED * FW_FLOW_PERIOD), bend, time, dpx, dpy, waveClass, vA);
        vec4 b = fwWaveSum(q - flow * (fb * FW_FLOW_SPEED * FW_FLOW_PERIOD) + vec2(5.3, 2.9), bend, time, dpx, dpy, waveClass, vB);
        wv = mix(b, a, wA);
        subVar = mix(vB, vA, wA);
        float chop = 1.0 + 0.8 * min(length(flow), 1.0);
        wv.xy *= chop;
        subVar *= chop * chop;
    } else {
        wv = fwWaveSum(q, bend, time, dpx, dpy, waveClass, subVar);
    }
    vec3 n = normalize(vec3(-wv.xy, 1.0));
    return vec4(n, wv.z / max(wv.w, 1e-3));
}

// Brightness of the body's light and of the mirrored surroundings, set side
// by side against Enhanced's Crystal Lake at 9:27 (docs/re/enhanced-water.md 4).
const float FW_BODY_AMB = 0.58;
// How much of the colours' own saturation the body keeps, and the green of
// the shore and trees in what the surface mirrors.
const float FW_ALBEDO_SAT = 0.15;
const vec3 FW_SURROUND_TINT = vec3(0.85, 1.0, 0.85);
const float FW_BODY_SUN = 0.05;
// The share of the view-depth difference the absorption sees (see
// docs/re/enhanced-water.md 4: Enhanced's lake bed shows through more water
// near the shore than its constants over our depth copy give).
const float FW_CLARITY = 0.7;
const vec3 FW_SHALLOW_TINT = vec3(0.8, 1.6, 1.05);
// How far the ripples' height moves the body between Color_2 and Color_1.
const float FW_CREST_COLOUR = 0.3;
const float FW_SURROUND_LIGHT = 0.6;
const float FW_SURROUND_SHARE = 0.75;
const float FW_ENV_SAT = 0.1;
// How much of the stand-in's edge is the ragged skyline (lakes, canals).
const float FW_SKYLINE = 0.6;
// The stand-in's brightness between the trees and under the tallest.
const float FW_TREE_GAP = 1.0;
const float FW_TREE_SHADE = 0.8;
const vec3 FW_BODY_TINT = vec3(0.95, 1.05, 0.97);
// The mirrored sky as a grey, a touch green, and dimmer than Light.dbc's
// rings (Enhanced's far lake reads 0.86 of ours in luma).
const vec3 FW_ENV_TINT = vec3(0.81, 0.83, 0.86);

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

// Enhanced's reflectionMode 2: screen-space reflections over its probe. The
// reflected ray is marched through the scene's depth copy (the scene before
// the water, so the water never hides what it mirrors) in six steps that
// double each time (to ~60 yards), and on the first crossing the hit is put
// between the last two points by their depth differences (a secant, no
// further taps) and the scene's colour read there: the shore, the trees and
// an island stand in the lake, broken up by the ripples. Returns the colour
// and its weight (0 = no hit; then the probe stand-in shows). Projected
// through the copy's own camera, as historyLookup does; the ray's clip
// position is linear in its length, so each step is four multiply-adds.
const int FW_SSR_STEPS = 6;
const float FW_SSR_MIN_F = 0.035;
vec4 fwScreenReflection(vec3 P, vec3 R, vec2 screenUV0, float near, float far) {
    vec4 c0 = projection * (view * vec4(P, 1.0));
    vec4 h0 = historyViewProj * vec4(P, 1.0);
    if (c0.w <= 1e-4 || h0.w <= 1e-4) return vec4(0.0);
    vec4 hR = historyViewProj * vec4(R, 0.0);
    vec2 ndcC = c0.xy / c0.w;
    float ySign = abs(screenUV0.y - (ndcC.y * 0.5 + 0.5)) <= abs(screenUV0.y - (0.5 - ndcC.y * 0.5)) ? 1.0 : -1.0;
    vec2 ndcH0 = h0.xy / h0.w;
    vec2 uvH0 = screenUV0 + vec2(ndcH0.x - ndcC.x, (ndcH0.y - ndcC.y) * ySign) * 0.5;
    float stepLen = 1.0;
    float t = 0.0;
    float bPrev = -1.0;
    for (int i = 0; i < FW_SSR_STEPS; ++i) {
        t += stepLen;
        vec4 hc = h0 + hR * t;
        if (hc.w <= 1e-3) return vec4(0.0);
        vec2 uv = uvH0 + vec2(hc.x / hc.w - ndcH0.x, (hc.y / hc.w - ndcH0.y) * ySign) * 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(0.0);
        float behind = hc.w - linearizeDepth(textureLod(SceneDepth, uv, 0.0).r, near, far);
        if (behind > 0.0) {
            // Crossed a surface: thick enough to be the thing in the way
            // (not a thin post the ray skipped past)?
            if (behind > max(2.0, stepLen * 1.5)) return vec4(0.0);
            float tHit = t - stepLen * behind / max(behind - bPrev, 1e-4);
            vec4 hh = h0 + hR * tHit;
            uv = uvH0 + vec2(hh.x / hh.w - ndcH0.x, (hh.y / hh.w - ndcH0.y) * ySign) * 0.5;
            // Fade toward the copy's edges and at the march's reach.
            vec2 e = smoothstep(vec2(0.0), vec2(0.06), uv) * smoothstep(vec2(1.0), vec2(0.94), uv);
            float reach = i < FW_SSR_STEPS - 1 ? 1.0 : 0.5;
            vec3 col = textureLod(SceneColor, clamp(uv, vec2(0.0), vec2(1.0)) * hdParams.zw, 0.0).rgb;
            return vec4(col, e.x * e.y * reach);
        }
        bPrev = behind;
        stepLen *= 2.0;
    }
    return vec4(0.0);
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
    // Enhanced's mode is its own build of this file (water_fw.frag.spv,
    // ENHANCED_WATER; the renderer binds it when hdParams.x is 2): each build
    // carries only its own path, so neither pays the other's registers - one
    // shader holding both ran the HD water slower whenever Enhanced's grew.
#ifdef ENHANCED_WATER
    const bool fw = true;
#else
    const bool fw = false;
#endif
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
    // Enhanced's wave class: 0 lakes and rivers, 1 the sea, 2 a building's
    // water (the canals).
    float waveClass = (push.depthRamp > 0.5 && push.depthRamp < 1.5) ? 1.0
                    : (push.depthRamp > 1.5 ? 2.0 : 0.0);
    float fwSubVar = 0.0;
    vec4 fwWave = fw ? fwSurfaceNormal(FragPos.xy, flowDir * flowSpeed, time, dist, waveClass, fwSubVar)
                     : vec4(0.0, 0.0, 1.0, 0.0);
    vec3 N = fw ? fwWave.xyz
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
            vec3 T = pow(exp(-t * FW_CLARITY * L.absorb * L.density), vec3(1.0 / 2.2));

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
            // Color_1 (crests) by the ripple's height, veiling in with the
            // depth, lit by the sky and the sun, forward-scattered toward a
            // low sun (HG). The height moves it only a little: at full
            // swing the colour followed the long waves' height and drew
            // broad light and dark bands, twice the capture's ripple
            // contrast.
            float hgt = clamp(0.5 + FW_CREST_COLOUR * fwWave.w, 0.0, 1.0);
            vec3 albedo = mix(L.colLow, L.colHigh, hgt);
            albedo = mix(vec3(dot(albedo, vec3(0.299, 0.587, 0.114))), albedo, FW_ALBEDO_SAT);
            float cosVL = dot(-viewDir, ldir);
            const float g = 0.6;
            float hg = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * cosVL, 1e-3), 1.5) * 0.25;
            // Scaled against Enhanced's own Crystal Lake (9:27): the deep
            // water there is a dark, low-chroma grey-teal that is mostly
            // the mirrored surroundings; the body under it is dim.
            // The sky light that reaches into the water, taken as its brightness
            // in the green-teal Enhanced's lakes read as (Light.dbc's ambient
            // is a blue the water's body does not take on in Enhanced).
            float ambL = dot(amb, vec3(0.299, 0.587, 0.114));
            vec3 inLight = ambL * FW_BODY_TINT * FW_BODY_AMB + sunIn * shadowF * (FW_BODY_SUN + FW_BODY_SUN * hg);
            // The body grows in over the depth as one veil, not per channel:
            // per channel, blue (gone within a yard) brought the body's full
            // blue over a red-brown bed and the shallows went mauve, where
            // Enhanced's go ochre, then olive, then grey-teal (the capture's
            // shallows read 102 80 50). The bed still loses blue first.
            float veil = 1.0 - dot(T, vec3(0.3, 0.55, 0.15));
            // Thin water over the bed reads olive in Enhanced's shallows
            // (G over R from a yard or two out); the veil starts in that
            // green and turns to the deep body's grey-teal as it thickens.
            vec3 bodyF = albedo * veil * inLight * mix(FW_SHALLOW_TINT, vec3(1.0), veil);

            // Reflection: EnvBRDFApprox at F0 0.02 of the environment -
            // Light.dbc's sky by the ring rule, or the planar sky pass.
            vec3 Nr = normalize(mix(vec3(0.0, 0.0, 1.0), N, 0.6));
            float NdotV = max(dot(Nr, viewDir), 1e-3);
            // Enhanced's roughness: a glassy base plus the root of the root
            // of the slope variance the pixel cannot show.
            float fwRough = clamp(max(0.06, 0.013 + sqrt(sqrt(fwSubVar * 0.5))), 0.0, 1.0);
            float F = fwEnvBrdf(fwRough, NdotV);
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
            // Enhanced mirrors a relit probe of the surroundings, not the sky
            // alone: low in the reflection are the shore, the trees and the
            // haze. Stand-in: toward the horizon the mirrored sky goes to the
            // fog colour, darkened as the land under it.
            float lowR = 1.0 - smoothstep(0.03, 0.6, R.z);
            float treeShade = 1.0;
            if (!isOcean) {
                // Round a lake or a canal the surroundings stand to uneven
                // heights (trees, banks, roofs): a ragged skyline by the
                // reflected ray's bearing, so the ripples break the mirror
                // into streaks of shore and sky as Enhanced's probe does,
                // instead of one smooth fade. The sea's horizon stays open.
                vec2 az = normalize(R.xy + vec2(1e-4));
                float n = fwNoise(az * 5.0 + 1.7) * 0.65 + fwNoise(az * 13.0 - 4.1) * 0.35;
                float skyline = mix(0.06, 0.5, n);
                lowR = mix(lowR, 1.0 - smoothstep(skyline - 0.04, skyline + 0.04, R.z), FW_SKYLINE);
                // Where the skyline stands tall the probe holds trees, darker
                // than the haze between them: streaks of shade in the far
                // water (the capture's far lake reads 82 99 99, the plain
                // stand-in 88 106 101).
                treeShade = mix(FW_TREE_GAP, FW_TREE_SHADE, n);
            }
            vec3 surround = mix(fogColor.rgb, amb, 0.4) * FW_SURROUND_TINT * FW_SURROUND_LIGHT * treeShade;
            env = mix(env, surround, lowR * FW_SURROUND_SHARE);
            // Enhanced's mirrored sky is a grey-blue: its probe is relit in
            // its linear light and graded; Light.dbc's sky rings are bluer.
            env = mix(vec3(dot(env, vec3(0.299, 0.587, 0.114))) * FW_ENV_TINT, env, FW_ENV_SAT);
            // What the screen holds of the surroundings, over the stand-in;
            // only where the mirror shows (F well over its 0.02 floor:
            // looking down at the water near the camera it does not).
            if (composeBed && F > FW_SSR_MIN_F) {
                vec4 ssr = fwScreenReflection(FragPos, R, screenUV, near, far);
                env = mix(env, ssr.rgb, ssr.a * smoothstep(FW_SSR_MIN_F, FW_SSR_MIN_F * 1.5, F));
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
            float ga = max(0.05, fwRough * fwRough);
            float a2 = ga * ga;
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
