#version 450

// Ambient occlusion - the "Ambient occlusion" setting (not stock;
// docs/re/enhanced-lighting.md section 4). Drawn by ContactShadowPass with
// this shader in place of contact_shadow.frag.
//
// the reference client draws Intel's ASSAO with the settings of its SSAOSettings row
// 116, which 260 of its 882 LightParams name: radius 1 yard, shadow multiplier
// 3, shadow power 3, shadow clamp 1, horizon threshold 0.1. This is ASSAO's
// obscurance term with that radius, threshold, clamp and power, in one pass:
//
//   per tap   max(0, dot(n, d / |d|) - 0.1) * max(0, 1 - |d|^2 / r^2)
//   pixel     o = min(mean(taps) * 4.3, 1) * fade
//   occlusion max((1 - o)^3, 0.6)
//
// The multiplier is ASSAO's default 1 rather than Enhanced's 3, and the
// ambient light keeps at least 0.6 of itself: Enhanced adds its probe GI over
// the result and Wowee has none (see kStrength). Taps sit on a fixed 24-point
// spiral 1 yard across on screen, six a pixel, pooled over the 2x2 quad as
// the contact shadows are; positions are rebuilt in view space. Only the
// light that comes from all around is occluded (how Enhanced's material
// shaders apply its AO was not traced; this is the usual use): the pixel's
// colour is multiplied by what is left of its light once that share
// is occluded - the ambient colour against the sun colour times N.L times
// the shadow map - blended 2 * dst * src (written at half, so the fill
// light below can brighten as well as darken).

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
    vec4 volumetricParams;
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    ivec4 localLightTileInfo;
    vec4 localLightTileScale;
    uvec4 localLightTiles[288];
    mat4 nearLightSpaceMatrix;
    vec4 nearShadowParams;
    uvec4 nearShadowMask[32];
};

#include "shadow_filter.glsli"

layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

layout(push_constant) uniform Push {
    mat4 invViewProj;  // clip -> world, this frame's
    vec4 target;       // xy the render extent, z unused (1), w debug
} pc;

layout(location = 0) out vec4 outColor;

// SSAOSettings 116 (Enhanced 1.60.1.70862).
const float kRadius = 1.0;           // yards
const float kHorizon = 0.1;          // horizon angle threshold
// Enhanced's ShadowMultiplier is 3 (3 * 4.3, clamped at 10 as ASSAO does).
// Side by side in Goldshire that took the whole road and every shaded bush
// down with it: Enhanced puts its probe GI back over the result and Wowee has
// none, and 3.3.5's ambient is a larger share of the light. ASSAO's own
// default multiplier, 1, matches what Enhanced's picture shows.
const float kStrength = 4.3;
const float kClamp = 1.0;            // ShadowClamp
const float kPower = 3.0;            // ShadowPower
// The most of the ambient light it takes. Enhanced's own result can reach
// none, but Enhanced adds its probe GI back on top (section 6); Wowee has no
// bounce light, so at full strength bushes and shaded walls went black.
const float kFloor = 0.6;
// Where it fades out. ASSAO's defaults are 50 to 300 yards; past 120 a
// 1-yard disk is under two pixels at 1080 lines, so it stops there.
const float kFadeFrom = 60.0;
// Fill light: how much of the horizon colour the sky gives an upward face,
// and how much of the sun the ground sends back up at a typical Elwynn grass
// and dirt colour. Tuned side by side against Enhanced's shade in Goldshire.
const float kFillSky = 0.35;
const float kFillBounce = 0.2;
const vec3 kGroundAlbedo = vec3(0.45, 0.42, 0.28);
const float kFadeTo = 120.0;
const int kTaps = 6;
const float kPlaneBias = 0.03;        // yards
const float kPlaneBiasPerYard = 0.003;
const float kQuadDepthYards = 0.1;
const float kQuadDepthPerYard = 0.02;

float eyeDistance(float d) {
    return projection[3][2] / (d + projection[2][2]);
}

// Positions in view space, from the linear depth: small numbers, so the
// differences between neighbouring pixels keep their precision. World
// coordinates in Elwynn are near 10^4 yards, where a float's step is a
// millimetre and the reconstructed facing came out in diagonal bands.
vec3 viewAt(ivec2 p) {
    float z = eyeDistance(texelFetch(uSceneDepth, p, 0).r);
    vec2 ndc = (vec2(p) + 0.5) / pc.target.xy * 2.0 - 1.0;
    return vec3(ndc.x * z / projection[0][0], ndc.y * z / projection[1][1], -z);
}

vec3 worldAt(ivec2 p) {
    float d = texelFetch(uSceneDepth, p, 0).r;
    vec2 uv = (vec2(p) + 0.5) / pc.target.xy;
    vec4 w = pc.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    return w.xyz / w.w;
}

float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

float otherH(float v, bool odd) { float d = dFdxFine(v); return odd ? v - d : v + d; }
float otherV(float v, bool odd) { float d = dFdyFine(v); return odd ? v - d : v + d; }

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    ivec2 size = ivec2(pc.target.xy);
    float d = texelFetch(uSceneDepth, px, 0).r;
    vec3 pos = worldAt(px);
    vec3 posV = viewAt(px);
    float dist = eyeDistance(d);

    // The facing from the nearer neighbour on each axis, so an edge takes
    // the surface the pixel is on rather than the one behind it.
    vec3 pl = viewAt(clamp(px - ivec2(1, 0), ivec2(0), size - 1));
    vec3 pr = viewAt(clamp(px + ivec2(1, 0), ivec2(0), size - 1));
    vec3 pd = viewAt(clamp(px - ivec2(0, 1), ivec2(0), size - 1));
    vec3 pu = viewAt(clamp(px + ivec2(0, 1), ivec2(0), size - 1));
    vec3 dx = (dot(pr - posV, pr - posV) < dot(posV - pl, posV - pl)) ? pr - posV : posV - pl;
    vec3 dy = (dot(pu - posV, pu - posV) < dot(posV - pd, posV - pd)) ? pu - posV : posV - pd;
    vec3 nV = cross(dx, dy);
    float nl2 = dot(nV, nV);
    nV *= inversesqrt(max(nl2, 1e-24));
    if (dot(nV, -posV) < 0.0) nV = -nV;
    // The same facing in world space, for the sun's share below.
    vec3 n = normalize(transpose(mat3(view)) * nV);

    // The disk's radius in pixels at this depth.
    float radiusPx = kRadius * abs(projection[1][1]) * 0.5 * pc.target.y / max(dist, 1e-3);
    int flags = int(pc.target.w + 0.5);
    bool aoOn = (flags & 1) != 0;
    bool fillOn = (flags & 2) != 0;
    bool working = aoOn && d < 1.0 && dist > 0.0 && dist < kFadeTo && nl2 >= 1e-12 && radiusPx >= 1.0;

    float sum = 0.0;
    if (working) {
        // No per-pixel spin: with no blur pass after it (ASSAO runs two),
        // a noise rotation showed as diagonal bands. The quad's pool of the
        // whole fixed spiral is smooth instead.
        bool oddX0 = (px.x & 1) != 0, oddY0 = (px.y & 1) != 0;
        int lane = (oddX0 ? 1 : 0) + (oddY0 ? 2 : 0);
        for (int k = 0; k < kTaps; ++k) {
            // A Vogel spiral of 4 * kTaps points; each pixel of the quad takes
            // every fourth, so the quad's pool covers the whole spiral.
            int i = lane + 4 * k;
            float r = sqrt((float(i) + 0.5) / float(4 * kTaps));
            float a = float(i) * 2.3999632;
            vec2 off = vec2(cos(a), sin(a)) * r * radiusPx;
            ivec2 q = clamp(px + ivec2(round(off)), ivec2(0), size - 1);
            if (q == px) continue;
            vec3 delta = viewAt(q) - posV;
            float len2 = dot(delta, delta);
            if (len2 < 1e-8) continue;
            // Height above the pixel's plane: below a few hundredths of a
            // yard (more with distance) it is the reconstruction's own noise,
            // not something standing over the surface.
            float h = dot(nV, delta);
            if (h < kPlaneBias + kPlaneBiasPerYard * dist) continue;
            float ndotd = h * inversesqrt(len2);
            float falloff = max(0.0, 1.0 - len2 / (kRadius * kRadius));
            sum += max(0.0, ndotd - kHorizon) * falloff;
        }
    }
    // Pool across the quad, as contact_shadow.frag does: a neighbour more
    // than a few inches off in depth is another surface and is not taken.
    bool oddX = (px.x & 1) != 0, oddY = (px.y & 1) != 0;
    float tol = kQuadDepthYards + kQuadDepthPerYard * dist;
    float ddA = working ? dist : -1e6;
    float wA = working ? 1.0 : 0.0;
    float sh = otherH(sum, oddX), dh = otherH(ddA, oddX);
    bool takeH = working && abs(dh - dist) < tol;
    float sumH = sum + (takeH ? sh : 0.0);
    float wH = wA + (takeH ? 1.0 : 0.0);
    float sv = otherV(sumH, oddY), wv = otherV(wH, oddY), dv = otherV(ddA, oddY);
    bool takeV = working && abs(dv - dist) < tol;
    float sumQ = sumH + (takeV ? sv : 0.0);
    float wQ = wH + (takeV ? wv : 0.0);
    if (d >= 1.0 || nl2 < 1e-12) discard;

    float occlusion = 1.0;
    if (working && wQ > 0.0) {
        float obscurance = sumQ / (wQ * float(kTaps));
        float fade = clamp((kFadeTo - dist) / (kFadeTo - kFadeFrom), 0.0, 1.0);
        obscurance = min(obscurance * kStrength, kClamp) * fade;
        occlusion = max(pow(clamp(1.0 - obscurance, 0.0, 1.0), kPower), kFloor);
    }
    if (!fillOn && occlusion >= 0.999) discard;

    if ((flags & 4) != 0) {
        // WOWEE_CONTACT_SHADOW_DEBUG (both passes read it): the occlusion,
        // grey (written at half: this pass blends 2 * dst * src).
        outColor = vec4(vec3(occlusion * 0.5), 1.0);
        return;
    }
    // The light at this pixel, per channel: the ambient (the light from all
    // around) and the sun's share, which keeps its value. Indoors the sun
    // is out of it (shadowParams.w, the indoor factor).
    float indoor = clamp(shadowParams.w, 0.0, 1.0);
    vec3 ldir = normalize(-lightDir.xyz);
    float ndotl = max(dot(n, ldir), 0.0);
    float sunLit = 1.0;
    if (shadowParams.x > 0.5 && ndotl > 0.0) sunLit = sunShadow(pos, n, ldir);
    vec3 amb = ambientColor.rgb;
    vec3 dif = lightColor.rgb * (ndotl * sunLit * (1.0 - indoor));
    vec3 ambNew = amb * occlusion;
    if (fillOn) {
        // Fill light (a stand-in for Enhanced's probe GI, section 6 of
        // docs/re/enhanced-lighting.md): the sky's light from above, in the
        // zone's horizon colour, and the sun's light bounced off the ground
        // from below, warmed by the ground's colour; a face takes each by how
        // far it turns toward it. Occluded as the ambient is. Outdoors only.
        float sunUp = smoothstep(0.0, 0.1, -normalize(lightDir.xyz).z);
        float up = clamp(n.z * 0.5 + 0.5, 0.0, 1.0);
        vec3 sky = fogColor.rgb * kFillSky;
        vec3 bounce = lightColor.rgb * kGroundAlbedo * (kFillBounce * sunUp);
        ambNew += (sky * up + bounce * (1.0 - up)) * (pc.target.z * occlusion * (1.0 - indoor));
    }
    vec3 before = amb + dif;
    vec3 f = clamp((ambNew + dif) / max(before, vec3(1e-3)), 0.0, 2.0);
    if (all(lessThan(abs(f - 1.0), vec3(0.002)))) discard;
    outColor = vec4(f * 0.5, 1.0);
}
