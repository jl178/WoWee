#version 450

// FXAA 3.11, quality preset 12 (Timothy Lottes). Reads the resolved scene
// colour and writes the smoothed result.
// Push constants: rcpFrame, sharpness, intoxication (0 = sober, 1 = smashed),
// ghost (0 = alive, 1 = the spirit world's desaturation) and antialias (0 when
// the pass runs only for the ghost world or intoxication, with FXAA off).
//
// A port of the reference rather than a paraphrase of it. The pass this
// replaces had the shape of FXAA and two of its numbers wrong. Its
// end-of-edge search compared each sample against lumaM minus the average of
// the two neighbours - a signed contrast, close to zero - where the reference
// subtracts the luma midway between the pixel and the neighbour across the
// edge, so the search ended on its first step nearly everywhere. The offset
// it then applied was dst/span where the reference has 0.5 - dst/span, so the
// middle of every edge got the full half-pixel blend and the stair-steps at
// the ends got none. Together that drew a half-pixel smear along every edge
// and removed no aliasing, which is what "FXAA makes everything blurry and
// does not help" looks like from the chair.

layout(set = 0, binding = 0) uniform sampler2D uScene;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec2  rcpFrame;
    float sharpness;    // 0 = no sharpen, 2 = max (matches FSR2 RCAS range)
    float intoxication;
    float ghost;
    float antialias;
    float rollWhite;    // Soft highlights (highlight_rolloff.hpp): the input that
    float rollRate;     // maps to 1 and the shoulder's rate; white 0 = off
    float glowAmount;   // LightParams.Glow: the blur, squared, added at this strength
    float glowBlend;    // how far the picture is swapped for its blur (water, drink)
    float glowSource;   // 1 when uGlow holds this frame's boxed-down scene
    float gradeWeight;  // Enhanced's colour grade (enhanced_grade.hpp): 0 = off
    float netherFade;   // ScreenEffect type 2: ffxnethercombine's c0.w, 0 = off
    float waveOn;       // 1 under liquid: the glow's last pass is ffxglowwave
    float waveU;        // the wave map's scroll, (t mod 3.174 s) / 3.174
    float waveV;        //                        (t mod 2.805 s) / 2.805
    float effectTime;   // seconds, for the nether blur's moving taps
    float gradeCurve;   // 0 the Eastern Kingdoms curve, 1 Kalimdor's
    float bloom;        // Enhanced bloom (not stock): strength, 0 = off
    float hdr;          // 1 when uScene is the RGBA16F HDR scene (hdrScene row)
    float toneKnee;     // HDR: where the tone curve leaves the identity
    float hdrBloom;     // HDR: how much light above 1 the bloom spreads
    float enhancedExposure;  // HDR + Enhanced daylight: the scene's light x this before
                            // the tone curve (enhanced_palette::kHdrExposure); 0 = off
    float enhancedSat;       // and the chroma scale below the shoulder (0 = off)
    vec4  enhancedShade;     // and the shade's: rgb the hue of the grey it keeps (the
                            // sky-lit ambient's, 1 = none), a its chroma scale (1 = as it is)
    vec4  cornerBlur;       // x 1 when uBlur holds this frame's corner sums (fxaa_blur.frag)
} pc;

// The scene boxed down to a quarter (mip 1 of a half-size image), the stock
// FFXBox4 target the glow and the ghost world blur from.
layout(set = 0, binding = 1) uniform sampler2D uGlow;
// Worked out from it once a frame at its texel corners (fxaa_blur.frag): rgb
// the glow's blur, a the brightest channel of the 8 x 8 texels round the
// corner. Read only while pc.cornerBlur.x is 1.
layout(set = 0, binding = 2) uniform sampler2D uBlur;

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// The scene as the rest of this pass reads it. The 8-bit scene arrives
// already clamped to 0..1 by its target. The HDR scene (RGBA16F, the "HDR
// lighting" row) holds whatever the shaders wrote, light above 1 included,
// and is tone mapped onto 0..1 here, at every tap, so FXAA's edge search,
// the stock glow and the grade work on the displayed picture as they always
// have. A negative or NaN value, which a float target keeps and an UNORM one
// clamps away, would spread through the blur taps; both are dropped.
//
// The curve (enhanced_tonemap.hpp holds the same numbers for the tests):
// identity up to the knee, so everything the 8-bit scene could show below
// it is shown as before; above it, Reinhard on the excess, x / (1 + x)
// scaled to the headroom left - slope 1 at the knee, 1 approached and never
// reached, so a highlight keeps its hue and gradient instead of clipping.
// Per channel: the brightest channel saturates first and a hot highlight
// bleaches toward white, as film and Enhanced's sun do.
//
// k + (1 - k) u / (1 + u) with u = (x - k) / (1 - k) is k + d h / (h + d),
// d = x - k and h = 1 - k: written that way it is one division and no
// select, which matters - this runs at every one of the pass's taps, and
// the select form cost about a millisecond at 1280x540 on Iris Xe.
vec3 hdrToDisplay(vec3 c) {
    c = max(c, vec3(0.0));             // also NaN -> 0 (max returns the non-NaN)
    float k = pc.toneKnee;
    float h = 1.0 - k;
    vec3 d = max(c - k, vec3(0.0));
    return min(c, vec3(k)) + d * h / (h + d);
}
float hdrToDisplay(float c) {
    c = max(c, 0.0);
    float k = pc.toneKnee;
    float h = 1.0 - k;
    float d = max(c - k, 0.0);
    return min(c, k) + d * h / (h + d);
}
// Its inverse on 0..1 (1 itself is out of reach and held just under it).
vec3 displayToHdr(vec3 d) {
    float k = pc.toneKnee;
    d = clamp(d, 0.0, 0.9990);
    vec3 f = max(d - k, 0.0) / (1.0 - k);
    vec3 lin = k + (1.0 - k) * f / (1.0 - f);
    return mix(d, lin, greaterThan(d, vec3(k)));
}
vec3 sceneAt(vec2 uv) {
    vec3 c = texture(uScene, uv).rgb;
    return pc.hdr > 0.5 ? hdrToDisplay(c) : c;
}
// FXAA's edge taps want only the luma: the tone curve on the luma alone,
// a third of the work, close enough for finding edges.
float lumaAt(vec2 uv) {
    float l = luma(texture(uScene, uv).rgb);
    return pc.hdr > 0.5 ? hdrToDisplay(l) : l;
}
// The same two at a whole pixel, clamped to the picture as the sampler's
// edge mode clamps. Where the pass reads the pixel itself and its eight
// neighbours, sober, its coordinates sit on texel centres, where the
// bilinear filter returns the texel unchanged: a fetch is the same value
// without the filter's work.
ivec2 texelIn(ivec2 p) {
    return clamp(p, ivec2(0), textureSize(uScene, 0) - 1);
}
vec3 sceneTexel(ivec2 p) {
    vec3 c = texelFetch(uScene, texelIn(p), 0).rgb;
    return pc.hdr > 0.5 ? hdrToDisplay(c) : c;
}
float lumaTexel(ivec2 p) {
    float l = luma(texelFetch(uScene, texelIn(p), 0).rgb);
    return pc.hdr > 0.5 ? hdrToDisplay(l) : l;
}
// A neighbour's luma, o pixels from this one.
float lumaNb(vec2 uv, ivec2 px, bool onTexel, ivec2 o) {
    return onTexel ? lumaTexel(px + o) : lumaAt(uv + vec2(o) * pc.rcpFrame);
}
// The quarter-size scene, as the glow and the nether blur read it
// (displayed), and as the bloom reads it (in the scene's own light).
vec3 glowAt(vec2 uv) {
    return textureLod(uGlow, uv, 1.0).rgb;
}
// A blur of the quarter-size scene, displayed: the HDR one is summed in the
// scene's light and mapped once, not tap by tap.
vec3 blurToDisplay(vec3 sum) {
    return pc.hdr > 0.5 ? hdrToDisplay(sum) : sum;
}
vec3 glowLinearAt(vec2 uv) {
    vec3 c = textureLod(uGlow, uv, 1.0).rgb;
    return pc.hdr > 0.5 ? max(c, vec3(0.0)) : c;
}

// The reference's default tuning. Subpix 0.75 is what it calls the default
// amount of filtering; 0.50 is the value it names as sharper, if this still
// reads soft on fine texture.
#define FXAA_EDGE_THRESHOLD      0.166   // local contrast below this is left alone
#define FXAA_EDGE_THRESHOLD_MIN  0.0833  // and darks below this, whatever their contrast
#define FXAA_SUBPIX              0.75    // sub-pixel aliasing removal, 0 = off
#define FXAA_SEARCH_STEPS        12

// Quality preset 12: fine steps next to the pixel, coarse ones far out along
// the edge. Eleven samples a side at most; the last step only positions.
const float kSearchStep[FXAA_SEARCH_STEPS] =
    float[](1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);


// The stock blur (FFXGauss4 across then down, on the quarter-size scene):
// four bilinear taps a side at -2.5, -0.5, +0.5, +2.5 quarter texels,
// weighted 1/8, 3/8, 3/8, 1/8. Taken as the 4x4 product in one pass, which
// is the same filter. Without a quarter-size scene, the pixel itself.
vec3 stockBlur(vec2 uv) {
    if (pc.glowSource < 0.5) return sceneAt(uv);
    if (pc.cornerBlur.x > 0.5) {
        // The same sum, from the corners: every tap sits half a texel off,
        // so the sum is bilinear between the quarter-size texels' corners,
        // and the sampler's one tap between the corner sums is all sixteen.
        // Corner k of a row is texel k of uBlur, centred at k + 0.5.
        vec2 q = vec2(textureSize(uGlow, 1));
        vec2 at = (uv * q + 0.5) / vec2(textureSize(uBlur, 0));
        return blurToDisplay(textureLod(uBlur, at, 0.0).rgb);
    }
    const float offs[4] = float[](-2.5, -0.5, 0.5, 2.5);
    const float wts[4]  = float[](0.125, 0.375, 0.375, 0.125);
    vec2 texel = 1.0 / vec2(textureSize(uGlow, 1));
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            sum += wts[x] * wts[y] *
                   glowAt(uv + vec2(offs[x], offs[y]) * texel);
        }
    }
    return blurToDisplay(sum);
}

// The glow wave's map (PassGlow.cpp 0x8c2920, "PassGlowWave", 128 x 128):
// along each axis a smooth wave, 1 - (6 - 4f) f^2 on the fraction of
// x / 64 - 0.5, negated on odd periods, to 0..1. Its coordinates (0x8c2350):
// the screen in pixels over 128 (the height times 0.88), turned 10 degrees
// (0xa942ac), scrolled by the clock's 3174 and 2805 ms periods.
float waveAxis(float x) {
    float u = x / 64.0 - 0.5;
    float n = floor(u);
    float f = u - n;
    float s = 1.0 - (6.0 - 4.0 * f) * f * f;
    if (mod(n, 2.0) >= 1.0) s = -s;
    return clamp(0.5 + 0.5 * s, 0.0, 1.0);
}
vec2 waveAt(vec2 uv) {
    vec2 size = vec2(textureSize(uScene, 0));
    vec2 tc = uv * vec2(size.x, size.y * 0.88) / 128.0;
    const float c = 0.98480775, s = 0.17364818;   // 10 degrees
    tc = vec2(c * tc.x - s * tc.y, s * tc.x + c * tc.y) + vec2(pc.waveU, pc.waveV);
    vec2 texel = fract(tc) * 128.0;
    return vec2(waveAxis(texel.x), waveAxis(texel.y));
}

// The nether world's blur (FFXNetherBlur): four taps of the quarter-size
// scene averaged. Stock moves the taps in its vertex program from random
// tables; here they turn about the pixel, 1.5 quarter texels out.
vec3 netherBlur(vec2 uv) {
    if (pc.glowSource < 0.5) return sceneAt(uv);
    vec2 texel = 1.0 / vec2(textureSize(uGlow, 1));
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float a = pc.effectTime * (0.7 + 0.3 * float(i)) + 1.5707963 * float(i);
        sum += glowAt(uv + vec2(cos(a), sin(a)) * 1.5 * texel);
    }
    return blurToDisplay(sum * 0.25);
}

// ffxnethercombine with c0 = (0.6, 0.6, 0.78, fade) (0xaf4940, 0x7e8c80).
vec3 netherCombine(vec3 scene, vec3 blur) {
    vec3 m = scene * 0.5 + blur;
    float g = dot(m, vec3(0.16666649));
    m = m * 0.5 + vec3(g);
    return scene + pc.netherFade * (m * vec3(0.6, 0.6, 0.78) - scene);
}

// What the stock client lays over the finished picture. Alive, ffxGlow
// (ffxglow.bls): lerp(scene, blur, blend) + blur^2 * glow. A ghost, ffxDeath
// (ffxdeath.bls) in its place: the scene plus the same glow, drained to grey
// by (0.299, 0.587, 0.144) - the shader's own 0.144 - and the cold tint
// (0x53, 0x93, 0xa8) laid on the mid-greys, 4 * grey * (1 - grey).
vec3 stockScreenEffectComposite(vec3 c, vec2 uv) {
    // The nether world's chain stands in for the glow (0x4f7020 type 2).
    if (pc.netherFade > 0.0) return netherCombine(c, netherBlur(uv));
    float g = clamp(pc.ghost, 0.0, 1.0);
    bool glow = pc.glowAmount > 0.0 || pc.glowBlend > 0.0;
    if (g <= 0.0 && !glow) return c;
    vec2 blurUV = uv;
    if (pc.waveOn > 0.5) {
        // ffxglowwave: the scene read 0..3 pixels off and the blur 0..0.75
        // quarter texels off along the wave (c0, c1 of 0x8c2350).
        vec2 w = waveAt(uv);
        c = sceneAt(uv + w * 3.0 * pc.rcpFrame);
        if (pc.glowSource > 0.5) blurUV = uv + w * 0.75 / vec2(textureSize(uGlow, 1));
    }
    vec3 blur = stockBlur(blurUV);
    vec3 alive = mix(c, blur, pc.glowBlend) + blur * blur * pc.glowAmount;
    if (g <= 0.0) return alive;
    vec3 lit = c + blur * blur * pc.glowAmount;
    float grey = clamp(dot(lit, vec3(0.299, 0.587, 0.144)), 0.0, 1.0);
    float t = clamp(4.0 * grey * (1.0 - grey), 0.0, 1.0);
    vec3 dead = vec3(grey) + vec3(83.0, 147.0, 168.0) / 255.0 * t;
    return mix(alive, dead, g);
}

// Soft highlights (not stock; highlight_rolloff.hpp has the derivation): the
// glow above can carry a channel past 1, where the stock write clips it flat.
// Each channel above the knee is eased onto [knee, 1] instead; a channel at
// or below it is returned as it came. Only pushed while the row is on and the
// glow is drawing.
vec3 highlightRollOff(vec3 x) {
    const float knee = 0.8;
    vec3 t = clamp((x - knee) / (pc.rollWhite - knee), 0.0, 1.0);
    vec3 s = pc.rollRate > 1e-4 ? (1.0 - exp(-pc.rollRate * t)) / (1.0 - exp(-pc.rollRate)) : t;
    return mix(x, knee + (1.0 - knee) * s, greaterThan(x, vec3(knee)));
}

// the reference client's daytime colour grade (not stock; enhanced_grade.hpp has the
// values and where they come from): one curve per channel, read from its LUTs
// 8286666 (Eastern Kingdoms) and 8286665 (Kalimdor), over the finished
// picture clamped to 0..1 as ffxcolorgrading.bls reads it. The weight is the
// hour's share of the noon keyframe that carries it, outdoors only.
const float kGradeEK[32] = float[](
    0.000, 0.031, 0.063, 0.094, 0.129, 0.165, 0.200, 0.235,
    0.267, 0.306, 0.341, 0.376, 0.412, 0.451, 0.490, 0.529,
    0.565, 0.604, 0.647, 0.686, 0.725, 0.765, 0.804, 0.851,
    0.890, 0.929, 0.957, 0.980, 0.988, 0.996, 0.996, 1.000);
const float kGradeKal[32] = float[](
    0.000, 0.031, 0.063, 0.094, 0.129, 0.165, 0.196, 0.231,
    0.263, 0.302, 0.337, 0.373, 0.408, 0.443, 0.482, 0.518,
    0.553, 0.588, 0.631, 0.667, 0.706, 0.741, 0.780, 0.824,
    0.863, 0.902, 0.937, 0.969, 0.988, 0.996, 0.996, 1.000);

// The same curves a segment at a time: each segment's two ends side by side,
// so a lookup is one load where it was two. A table indexed per pixel is
// read from memory, not registers, and the four reads a channel the select
// between the continents made (both tables, both ends) cost a third of this
// pass on the Iris Xe.
const vec2 kGradeSegEK[31] = vec2[](
    vec2(kGradeEK[0], kGradeEK[1]), vec2(kGradeEK[1], kGradeEK[2]), vec2(kGradeEK[2], kGradeEK[3]),
    vec2(kGradeEK[3], kGradeEK[4]), vec2(kGradeEK[4], kGradeEK[5]), vec2(kGradeEK[5], kGradeEK[6]),
    vec2(kGradeEK[6], kGradeEK[7]), vec2(kGradeEK[7], kGradeEK[8]), vec2(kGradeEK[8], kGradeEK[9]),
    vec2(kGradeEK[9], kGradeEK[10]), vec2(kGradeEK[10], kGradeEK[11]), vec2(kGradeEK[11], kGradeEK[12]),
    vec2(kGradeEK[12], kGradeEK[13]), vec2(kGradeEK[13], kGradeEK[14]), vec2(kGradeEK[14], kGradeEK[15]),
    vec2(kGradeEK[15], kGradeEK[16]), vec2(kGradeEK[16], kGradeEK[17]), vec2(kGradeEK[17], kGradeEK[18]),
    vec2(kGradeEK[18], kGradeEK[19]), vec2(kGradeEK[19], kGradeEK[20]), vec2(kGradeEK[20], kGradeEK[21]),
    vec2(kGradeEK[21], kGradeEK[22]), vec2(kGradeEK[22], kGradeEK[23]), vec2(kGradeEK[23], kGradeEK[24]),
    vec2(kGradeEK[24], kGradeEK[25]), vec2(kGradeEK[25], kGradeEK[26]), vec2(kGradeEK[26], kGradeEK[27]),
    vec2(kGradeEK[27], kGradeEK[28]), vec2(kGradeEK[28], kGradeEK[29]), vec2(kGradeEK[29], kGradeEK[30]),
    vec2(kGradeEK[30], kGradeEK[31]));
const vec2 kGradeSegKal[31] = vec2[](
    vec2(kGradeKal[0], kGradeKal[1]), vec2(kGradeKal[1], kGradeKal[2]), vec2(kGradeKal[2], kGradeKal[3]),
    vec2(kGradeKal[3], kGradeKal[4]), vec2(kGradeKal[4], kGradeKal[5]), vec2(kGradeKal[5], kGradeKal[6]),
    vec2(kGradeKal[6], kGradeKal[7]), vec2(kGradeKal[7], kGradeKal[8]), vec2(kGradeKal[8], kGradeKal[9]),
    vec2(kGradeKal[9], kGradeKal[10]), vec2(kGradeKal[10], kGradeKal[11]), vec2(kGradeKal[11], kGradeKal[12]),
    vec2(kGradeKal[12], kGradeKal[13]), vec2(kGradeKal[13], kGradeKal[14]), vec2(kGradeKal[14], kGradeKal[15]),
    vec2(kGradeKal[15], kGradeKal[16]), vec2(kGradeKal[16], kGradeKal[17]), vec2(kGradeKal[17], kGradeKal[18]),
    vec2(kGradeKal[18], kGradeKal[19]), vec2(kGradeKal[19], kGradeKal[20]), vec2(kGradeKal[20], kGradeKal[21]),
    vec2(kGradeKal[21], kGradeKal[22]), vec2(kGradeKal[22], kGradeKal[23]), vec2(kGradeKal[23], kGradeKal[24]),
    vec2(kGradeKal[24], kGradeKal[25]), vec2(kGradeKal[25], kGradeKal[26]), vec2(kGradeKal[26], kGradeKal[27]),
    vec2(kGradeKal[27], kGradeKal[28]), vec2(kGradeKal[28], kGradeKal[29]), vec2(kGradeKal[29], kGradeKal[30]),
    vec2(kGradeKal[30], kGradeKal[31]));

// One channel through a curve: segment i of 31, f of the way along it.
float gradeEK(float x) {
    float t = clamp(x, 0.0, 1.0) * 31.0;
    int i = min(int(t), 30);
    vec2 ab = kGradeSegEK[i];
    return mix(ab.x, ab.y, t - float(i));
}
float gradeKal(float x) {
    float t = clamp(x, 0.0, 1.0) * 31.0;
    int i = min(int(t), 30);
    vec2 ab = kGradeSegKal[i];
    return mix(ab.x, ab.y, t - float(i));
}

vec3 enhancedGrade(vec3 c) {
    // Mid-tones only. Enhanced's curve lifts the upper mid-tones most (0.77 ->
    // 0.89) of a tone-mapped picture whose highlights are already rolled
    // off; here the sunlit ground and leaves sit at 0.7-0.9 and the lift
    // drove them to white. Fades out from 0.6 to 0.9 of the brightest
    // channel, so a highlight keeps the value it has - and is not looked up.
    float top = max(c.r, max(c.g, c.b));
    float w = pc.gradeWeight * (1.0 - smoothstep(0.6, 0.9, top));
    if (w <= 0.0) return c;
    // The continent is the same for every pixel: one table is read.
    vec3 g;
    if (pc.gradeCurve > 0.5) {
        g = vec3(gradeKal(c.r), gradeKal(c.g), gradeKal(c.b));
    } else {
        g = vec3(gradeEK(c.r), gradeEK(c.g), gradeEK(c.b));
    }
    return mix(c, g, w);
}

// Bloom (not stock; the Enhanced preset's "Bloom Intensity", which Enhanced
// ships at its maximum): what is brighter than kBloomKnee in a wide tent of
// the quarter-size scene (nine bilinear taps four texels apart), spread a
// few dozen pixels and added on. The stock
// glow above squares its blur, which brightens everything a little; this
// lifts only the highlights - sunlit leaves, the sky round the sun, the
// shafts - into a soft halo.
const float kBloomKnee = 0.8;
const float kHdrBloomFloor = 1.6;
const float kHdrBloomMax = 0.3;
// The two at once, from one set of taps: x the 8-bit bloom over the
// displayed picture, as the Enhanced preset has always drawn it; y, with the
// HDR scene only, the light above 1 in the same tent, in the scene's light -
// the sky round the sun, a sunlit leaf - which the 8-bit scene had clipped.
void bloomAt(vec2 uv, out vec3 ldr, out vec3 hdr) {
    ldr = vec3(0.0);
    hdr = vec3(0.0);
    if (pc.glowSource < 0.5) return;
    vec2 texel = 1.0 / vec2(textureSize(uGlow, 1));
    // Each scene takes only its own half: the 8-bit halo is drawn over the
    // 8-bit scene alone and the light above the floor is the HDR scene's
    // alone (an 8-bit tap never passes 1). Working out both at every tap
    // put nine tone maps a pixel on the HDR scene for a halo it discards.
    bool hdrScene = pc.hdr > 0.5;
    if (hdrScene && pc.cornerBlur.x > 0.5) {
        // The taps below reach texels from 3 before the nearest corner to 2
        // after it; that corner's alpha is the brightest channel from 4
        // before to 3 after. Under the floor there, every tap is under it
        // and adds nothing - which is nearly every pixel.
        vec2 q = vec2(textureSize(uGlow, 1));
        ivec2 corner = clamp(ivec2(round(uv * q)), ivec2(0), textureSize(uBlur, 0) - 1);
        float peak = texelFetch(uBlur, corner, 0).a;
        if (peak < kHdrBloomFloor - 0.01) return;
    }
    // With the HDR scene the bloom is only the light above the floor
    // (a lamp, a candle, the sun): spread tight, a texel and a half
    // of the quarter-size scene apart, so a torch keeps a small local
    // glow rather than a wide halo (Jered's Goldshire candles).
    float spread = hdrScene ? 1.5 : 4.0;
    // The tent's weights, (2 - |x|)(2 - |y|), sum to 16.
    const float wsum = 16.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            float w = (2.0 - abs(float(x))) * (2.0 - abs(float(y)));
            vec3 s = glowLinearAt(uv + vec2(x, y) * spread * texel);
            if (hdrScene) {
                // Light above kHdrBloomFloor: the sun's disc and glare (2.5x),
                // lamps and fire. At 1 the sunlit Barrens ground under an HDR
                // sun bloomed into a glowing orange-white sheet.
                hdr += max(s - kHdrBloomFloor, 0.0) * w;
            } else {
                ldr += max(s - kBloomKnee, 0.0) * w;
            }
        }
    }
    ldr = ldr / wsum / (1.0 - kBloomKnee);
    hdr = hdr / wsum;
}

vec3 stockScreenEffect(vec3 c, vec2 uv) {
    vec3 r = stockScreenEffectComposite(c, uv);
    vec3 ldr = vec3(0.0), hdr = vec3(0.0);
    if (pc.bloom > 0.0) {
        bloomAt(uv, ldr, hdr);
        // Capped: a backlit, haze-lit face beside the sun already sits near
        // white, and the halo on top of it was what blew it out. And
        // screened rather than added: what is already near white takes
        // almost none of the halo, so it glows into the darker picture round
        // it instead of piling onto the sky and the leaves by the sun until
        // the top of the screen was one white-yellow sheet (Jered's
        // Goldshire road).
        // With the HDR scene the light above its floor is the bloom; the
        // 8-bit halo over the displayed picture's top fifth would bloom every
        // sunlit wall and road, so it is left to the 8-bit scene.
        if (pc.hdr < 0.5) r += min(ldr * pc.bloom, vec3(0.25)) * clamp(1.0 - r, 0.0, 1.0);
    }
    if (pc.hdr > 0.5) {
        // The HDR scene: what the glow and the bloom added on the displayed
        // picture goes on in the scene's light instead, with the light above
        // 1 the bloom spreads (a halo that grows with how bright its source
        // is), and the sum is tone mapped once - so a lit face under the
        // glow eases toward white on the shoulder instead of clipping there.
        // The bloom's light is held to kHdrBloomMax a channel: additive
        // spell effects (Holy Light, Consecration, a level-up) and lamp cards
        // pile up far past 1 in the HDR scene, where the 8-bit one stopped
        // each at 1, and their bloom grew into a large halo round the player.
        // Held, a bright effect keeps a small, tight glow at most.
        vec3 lin = displayToHdr(c) + (r - c) + min(hdr * (pc.bloom * pc.hdrBloom), vec3(kHdrBloomMax));
        // Enhanced's lift of the lit mid-tones: its sunlit ground reads
        // brighter against the same shade (lit/shade 2.1-2.3 at Goldshire
        // 9:22). A band from 0.2 to 0.8 of the light's luminance - the shade
        // keeps its depth and the top its value, so a sunlit Barrens road
        // stays textured (a straight exposure took it to a glowing orange; a
        // lift of the low end greyed the shade).
        if (pc.enhancedExposure > 0.0) {
            float y = dot(lin, vec3(0.2126, 0.7152, 0.0722));
            // ...and by the brightest channel too: the Barrens' red dirt has
            // a modest luminance but its red near the top.
            float hiCh = max(lin.r, max(lin.g, lin.b));
            float band = smoothstep(0.2, 0.35, y) * (1.0 - smoothstep(0.55, 0.8, y)) *
                         (1.0 - smoothstep(0.7, 0.9, hiCh));
            lin *= 1.0 + (pc.enhancedExposure - 1.0) * band;
        }
        r = hdrToDisplay(lin);
        // On the shoulder each channel eases toward 1 on its own, so a
        // saturated sky keeps its full chroma up to the top (Duskwood's
        // noon cyan). A highlight's colour eases toward its grey instead,
        // as a film shoulder does, by how far into the shoulder it is.
        // And Enhanced's palette: its sunlit ground and fences warm and
        // saturated (Goldshire 9:22), its shade a deeper, near-neutral green -
        // more chroma on lit surfaces, easing to less at the very top.
        if (pc.enhancedSat > 0.0) {
            float top = max(r.r, max(r.g, r.b));
            float y = dot(r, vec3(0.2126, 0.7152, 0.0722));
            // Lit surfaces only: Enhanced's shade is a near-neutral green
            // (chroma 0.46-0.57) under its saturated sunlit ground (0.62-0.65).
            float k = mix(1.0, pc.enhancedSat, smoothstep(0.25, 0.45, y));
            // And the shade less (enhanced_palette::kHdrShadeChroma): 3.3.5's
            // leaf, grass and dirt textures carry almost no blue, so under a
            // sky-blue ambient the shade stays a saturated yellow-green
            // (shade b/luminance 0.41-0.52) where Enhanced's, under the same
            // light data, is a near-neutral warm grey (0.71-0.73). By day,
            // outdoors; full below 0.18 luminance, none from 0.38, and only
            // where the shade is warm (blue short of red and green): a shade
            // that already has its blue - Duskwood's teal - keeps its colour.
            // The grey it keeps is cooler: the ambient's blue against its red
            // and green (enhanced_palette::shadeTint) - Enhanced's sky light.
            float shadeW = (1.0 - smoothstep(0.18, 0.38, y)) *
                           smoothstep(0.0, 0.1, 0.5 * (r.r + r.g) - r.b);
            k *= mix(1.0, pc.enhancedShade.a, shadeW);
            k = mix(k, 0.8, smoothstep(0.75, 0.95, top));
            vec3 grey = vec3(y) * mix(vec3(1.0), pc.enhancedShade.rgb, shadeW);
            r = max(grey + (r - vec3(y)) * k, vec3(0.0));
        }
    } else if (pc.rollWhite > 0.0) {
        // Soft highlights belong to the 8-bit scene; the tone curve does it
        // for the HDR one.
        r = highlightRollOff(r);
    }
    if (pc.gradeWeight > 0.0) r = enhancedGrade(r);
    // Dithered before the 8-bit write (the Enhanced rows only): the tone
    // curve's shoulder and the grade spread a sky's smooth gradient over a
    // few levels, which showed as rings round the sun. A triangular dither
    // of one level, from interleaved gradient noise, breaks the steps up.
    if (pc.hdr > 0.5 || pc.bloom > 0.0 || pc.gradeWeight > 0.0) {
        vec2 f = gl_FragCoord.xy;
        float n1 = fract(52.9829189 * fract(dot(f, vec2(0.06711056, 0.00583715))));
        float n2 = fract(52.9829189 * fract(dot(f + vec2(17.0, 59.0), vec2(0.06711056, 0.00583715))));
        r += (n1 + n2 - 1.0) / 255.0;
    }
    return r;
}

vec3 fxaa(vec2 uv, ivec2 px, bool onTexel, vec3 rgbM, float drunk);

void main() {
    float drunk = clamp(pc.intoxication, 0.0, 1.0);
    vec2 uv     = TexCoord + vec2(
        sin(TexCoord.y * 34.0) * pc.rcpFrame.x,
        cos(TexCoord.x * 29.0) * pc.rcpFrame.y) * (3.0 * drunk);
    vec2 rcp    = pc.rcpFrame;

    // Sober and at the scene's own size, this pixel and its neighbours are
    // whole texels (lumaNb).
    ivec2 px = ivec2(gl_FragCoord.xy);
    bool onTexel = drunk <= 0.0 && textureSize(uScene, 0) == ivec2(round(1.0 / rcp));

    // --- Centre, with the drunk blur folded in before anything reads it ---
    vec3 rgbM = onTexel ? sceneTexel(px) : sceneAt(uv);
    if (drunk > 0.0) {
        vec2 radius = rcp * mix(1.0, 5.0, drunk);
        vec3 blur = rgbM;
        blur += sceneAt(uv + vec2( radius.x, 0.0));
        blur += sceneAt(uv + vec2(-radius.x, 0.0));
        blur += sceneAt(uv + vec2(0.0,  radius.y));
        blur += sceneAt(uv + vec2(0.0, -radius.y));
        blur += sceneAt(uv + radius);
        blur += sceneAt(uv - radius);
        blur += sceneAt(uv + vec2(radius.x, -radius.y));
        blur += sceneAt(uv + vec2(-radius.x, radius.y));
        rgbM = mix(rgbM, blur / 9.0, 0.75 * drunk);
    }

    // FXAA switched off: the pass is here for the colour alone.
    vec3 c = pc.antialias < 0.5 ? rgbM : fxaa(uv, px, onTexel, rgbM, drunk);
    // One call for every way through: written out after each of FXAA's
    // exits, it was inlined three times over, and the shader's size (its
    // registers) cost the pass more than its edge search did.
    outColor = vec4(stockScreenEffect(c, uv), 1.0);
}

// FXAA over the pixel rgbM (sober: texel px); rgbM itself where there is no
// edge to smooth.
vec3 fxaa(vec2 uv, ivec2 px, bool onTexel, vec3 rgbM, float drunk) {
    vec2 rcp = pc.rcpFrame;
    // --- Cardinal neighbours: is there an edge here at all ---
    float lumaM = luma(rgbM);
    float lumaN = lumaNb(uv, px, onTexel, ivec2( 0, -1));
    float lumaS = lumaNb(uv, px, onTexel, ivec2( 0,  1));
    float lumaE = lumaNb(uv, px, onTexel, ivec2( 1,  0));
    float lumaW = lumaNb(uv, px, onTexel, ivec2(-1,  0));

    float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float range    = rangeMax - rangeMin;

    if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD)) {
        return rgbM;
    }

    float lumaNW = lumaNb(uv, px, onTexel, ivec2(-1, -1));
    float lumaNE = lumaNb(uv, px, onTexel, ivec2( 1, -1));
    float lumaSW = lumaNb(uv, px, onTexel, ivec2(-1,  1));
    float lumaSE = lumaNb(uv, px, onTexel, ivec2( 1,  1));

    float lumaNS   = lumaN + lumaS;
    float lumaWE   = lumaW + lumaE;
    float lumaNWSW = lumaNW + lumaSW;
    float lumaNESE = lumaNE + lumaSE;
    float lumaNWNE = lumaNW + lumaNE;
    float lumaSWSE = lumaSW + lumaSE;

    // --- Sub-pixel aliasing: how far the pixel sits from its 3x3 lowpass ---
    float subpixA = (lumaNS + lumaWE) * 2.0 + (lumaNWSW + lumaNESE);
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float subpixC = clamp(abs(subpixB) / range, 0.0, 1.0);
    float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;   // smoothstep
    float subpixH = subpixF * subpixF * FXAA_SUBPIX;

    // --- Edge orientation ---
    float edgeHorz = abs(-2.0 * lumaW + lumaNWSW)
                   + abs(-2.0 * lumaM + lumaNS) * 2.0
                   + abs(-2.0 * lumaE + lumaNESE);
    float edgeVert = abs(-2.0 * lumaS + lumaSWSE)
                   + abs(-2.0 * lumaM + lumaWE) * 2.0
                   + abs(-2.0 * lumaN + lumaNWNE);
    bool horzSpan = edgeHorz >= edgeVert;

    // The neighbour across the edge is the one with the larger gradient from
    // the pixel; lengthSign points at it.
    float luma1 = horzSpan ? lumaN : lumaW;
    float luma2 = horzSpan ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;
    bool  pair1 = abs(gradient1) >= abs(gradient2);
    float gradientScaled = max(abs(gradient1), abs(gradient2)) * 0.25;
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    if (pair1) lengthSign = -lengthSign;

    // The luma half-way between the pixel and that neighbour. The search walks
    // along the edge, half a pixel over onto the line between the two, and a
    // side is done where its sample leaves this midpoint by more than a
    // quarter of the gradient: the end of the edge on that side.
    float lumaMid     = ((pair1 ? luma1 : luma2) + lumaM) * 0.5;
    bool  lumaMLTZero = (lumaM - lumaMid) < 0.0;

    vec2 posB  = uv;
    vec2 offNP = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    if ( horzSpan) posB.y += lengthSign * 0.5;
    if (!horzSpan) posB.x += lengthSign * 0.5;

    vec2  posN = posB - offNP * kSearchStep[0];
    vec2  posP = posB + offNP * kSearchStep[0];
    float lumaEndN = lumaAt(posN) - lumaMid;
    float lumaEndP = lumaAt(posP) - lumaMid;
    bool  doneN = abs(lumaEndN) >= gradientScaled;
    bool  doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kSearchStep[1];
    if (!doneP) posP += offNP * kSearchStep[1];
    for (int i = 2; i < FXAA_SEARCH_STEPS && !(doneN && doneP); ++i) {
        if (!doneN) lumaEndN = lumaAt(posN) - lumaMid;
        if (!doneP) lumaEndP = lumaAt(posP) - lumaMid;
        doneN = doneN || abs(lumaEndN) >= gradientScaled;
        doneP = doneP || abs(lumaEndP) >= gradientScaled;
        if (!doneN) posN -= offNP * kSearchStep[i];
        if (!doneP) posP += offNP * kSearchStep[i];
    }

    // --- Where along the span this pixel is, and how much to blend for it ---
    float dstN = horzSpan ? (uv.x - posN.x) : (uv.y - posN.y);
    float dstP = horzSpan ? (posP.x - uv.x) : (posP.y - uv.y);
    bool  directionN = dstN < dstP;
    float dst        = min(dstN, dstP);
    float spanLength = dstN + dstP;
    bool  goodSpanN  = (lumaEndN < 0.0) != lumaMLTZero;
    bool  goodSpanP  = (lumaEndP < 0.0) != lumaMLTZero;
    bool  goodSpan   = directionN ? goodSpanN : goodSpanP;

    // Half a pixel at the near end of the span, where the stair-step is,
    // falling to nothing at its middle, where the edge is already straight.
    float pixelOffset      = 0.5 - dst / spanLength;
    float pixelOffsetGood  = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetFinal = max(pixelOffsetGood, subpixH);

    vec2 finalUV = uv;
    if ( horzSpan) finalUV.y += pixelOffsetFinal * lengthSign;
    if (!horzSpan) finalUV.x += pixelOffsetFinal * lengthSign;

    vec3 fxaaResult = sceneAt(finalUV);
    fxaaResult = mix(fxaaResult, rgbM, 0.75 * drunk);

    // Post-FXAA contrast-adaptive sharpening (unsharp mask), only when FSR2
    // hands over its sharpness: it restores what the upscale softened.
    if (pc.sharpness > 0.0) {
        vec2 r = pc.rcpFrame;
        vec3 blur = (sceneAt(uv + vec2(-r.x, 0))
                   + sceneAt(uv + vec2( r.x, 0))
                   + sceneAt(uv + vec2(0, -r.y))
                   + sceneAt(uv + vec2(0,  r.y))) * 0.25;
        // scale sharpness from [0,2] to a modest [0, 0.3] boost factor
        float s = pc.sharpness * 0.15;
        fxaaResult = clamp(fxaaResult + s * (fxaaResult - blur), 0.0, 1.0);
    }

    return fxaaResult;
}
