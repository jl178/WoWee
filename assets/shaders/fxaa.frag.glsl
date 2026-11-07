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
    float pad2;
    float netherFade;   // ScreenEffect type 2: ffxnethercombine's c0.w, 0 = off
    float waveOn;       // 1 under liquid: the glow's last pass is ffxglowwave
    float waveU;        // the wave map's scroll, (t mod 3.174 s) / 3.174
    float waveV;        //                        (t mod 2.805 s) / 2.805
    float effectTime;   // seconds, for the nether blur's moving taps
    float pad3;
    float pad4;
    float pad5;
} pc;

// The scene boxed down to a quarter (mip 1 of a half-size image), the stock
// FFXBox4 target the glow and the ghost world blur from.
layout(set = 0, binding = 1) uniform sampler2D uGlow;

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

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// The stock blur (FFXGauss4 across then down, on the quarter-size scene):
// four bilinear taps a side at -2.5, -0.5, +0.5, +2.5 quarter texels,
// weighted 1/8, 3/8, 3/8, 1/8. Taken as the 4x4 product in one pass, which
// is the same filter. Without a quarter-size scene, the pixel itself.
vec3 stockBlur(vec2 uv) {
    if (pc.glowSource < 0.5) return texture(uScene, uv).rgb;
    const float offs[4] = float[](-2.5, -0.5, 0.5, 2.5);
    const float wts[4]  = float[](0.125, 0.375, 0.375, 0.125);
    vec2 texel = 1.0 / vec2(textureSize(uGlow, 1));
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            sum += wts[x] * wts[y] *
                   textureLod(uGlow, uv + vec2(offs[x], offs[y]) * texel, 1.0).rgb;
        }
    }
    return sum;
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
    if (pc.glowSource < 0.5) return texture(uScene, uv).rgb;
    vec2 texel = 1.0 / vec2(textureSize(uGlow, 1));
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        float a = pc.effectTime * (0.7 + 0.3 * float(i)) + 1.5707963 * float(i);
        sum += textureLod(uGlow, uv + vec2(cos(a), sin(a)) * 1.5 * texel, 1.0).rgb;
    }
    return sum * 0.25;
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
        c = texture(uScene, uv + w * 3.0 * pc.rcpFrame).rgb;
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

vec3 stockScreenEffect(vec3 c, vec2 uv) {
    vec3 r = stockScreenEffectComposite(c, uv);
    if (pc.rollWhite > 0.0) r = highlightRollOff(r);
    return r;
}

void main() {
    float drunk = clamp(pc.intoxication, 0.0, 1.0);
    vec2 uv     = TexCoord + vec2(
        sin(TexCoord.y * 34.0) * pc.rcpFrame.x,
        cos(TexCoord.x * 29.0) * pc.rcpFrame.y) * (3.0 * drunk);
    vec2 rcp    = pc.rcpFrame;

    // --- Centre, with the drunk blur folded in before anything reads it ---
    vec3 rgbM = texture(uScene, uv).rgb;
    if (drunk > 0.0) {
        vec2 radius = rcp * mix(1.0, 5.0, drunk);
        vec3 blur = rgbM;
        blur += texture(uScene, uv + vec2( radius.x, 0.0)).rgb;
        blur += texture(uScene, uv + vec2(-radius.x, 0.0)).rgb;
        blur += texture(uScene, uv + vec2(0.0,  radius.y)).rgb;
        blur += texture(uScene, uv + vec2(0.0, -radius.y)).rgb;
        blur += texture(uScene, uv + radius).rgb;
        blur += texture(uScene, uv - radius).rgb;
        blur += texture(uScene, uv + vec2(radius.x, -radius.y)).rgb;
        blur += texture(uScene, uv + vec2(-radius.x, radius.y)).rgb;
        rgbM = mix(rgbM, blur / 9.0, 0.75 * drunk);
    }

    // FXAA switched off: the pass is here for the colour alone.
    if (pc.antialias < 0.5) {
        outColor = vec4(stockScreenEffect(rgbM, uv), 1.0);
        return;
    }

    // --- Cardinal neighbours: is there an edge here at all ---
    float lumaM = luma(rgbM);
    float lumaN = luma(texture(uScene, uv + vec2( 0.0, -1.0) * rcp).rgb);
    float lumaS = luma(texture(uScene, uv + vec2( 0.0,  1.0) * rcp).rgb);
    float lumaE = luma(texture(uScene, uv + vec2( 1.0,  0.0) * rcp).rgb);
    float lumaW = luma(texture(uScene, uv + vec2(-1.0,  0.0) * rcp).rgb);

    float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    float range    = rangeMax - rangeMin;

    if (range < max(FXAA_EDGE_THRESHOLD_MIN, rangeMax * FXAA_EDGE_THRESHOLD)) {
        outColor = vec4(stockScreenEffect(rgbM, uv), 1.0);
        return;
    }

    float lumaNW = luma(texture(uScene, uv + vec2(-1.0, -1.0) * rcp).rgb);
    float lumaNE = luma(texture(uScene, uv + vec2( 1.0, -1.0) * rcp).rgb);
    float lumaSW = luma(texture(uScene, uv + vec2(-1.0,  1.0) * rcp).rgb);
    float lumaSE = luma(texture(uScene, uv + vec2( 1.0,  1.0) * rcp).rgb);

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
    float lumaEndN = luma(texture(uScene, posN).rgb) - lumaMid;
    float lumaEndP = luma(texture(uScene, posP).rgb) - lumaMid;
    bool  doneN = abs(lumaEndN) >= gradientScaled;
    bool  doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * kSearchStep[1];
    if (!doneP) posP += offNP * kSearchStep[1];
    for (int i = 2; i < FXAA_SEARCH_STEPS && !(doneN && doneP); ++i) {
        if (!doneN) lumaEndN = luma(texture(uScene, posN).rgb) - lumaMid;
        if (!doneP) lumaEndP = luma(texture(uScene, posP).rgb) - lumaMid;
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

    vec3 fxaaResult = texture(uScene, finalUV).rgb;
    fxaaResult = mix(fxaaResult, rgbM, 0.75 * drunk);

    // Post-FXAA contrast-adaptive sharpening (unsharp mask), only when FSR2
    // hands over its sharpness: it restores what the upscale softened.
    if (pc.sharpness > 0.0) {
        vec2 r = pc.rcpFrame;
        vec3 blur = (texture(uScene, uv + vec2(-r.x, 0)).rgb
                   + texture(uScene, uv + vec2( r.x, 0)).rgb
                   + texture(uScene, uv + vec2(0, -r.y)).rgb
                   + texture(uScene, uv + vec2(0,  r.y)).rgb) * 0.25;
        // scale sharpness from [0,2] to a modest [0, 0.3] boost factor
        float s = pc.sharpness * 0.15;
        fxaaResult = clamp(fxaaResult + s * (fxaaResult - blur), 0.0, 1.0);
    }

    outColor = vec4(stockScreenEffect(fxaaResult, uv), 1.0);
}
