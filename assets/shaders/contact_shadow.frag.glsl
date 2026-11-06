#version 450

// Contact shadows - the "Contact shadows" setting (not stock;
// docs/enhancements-plan.md E14, rendering/contact_shadows.hpp keeps the same
// constants and the colour factor, tested in test_phase2_light).
//
// A full-screen pass over the scene's resolved colour once the scene pass has
// closed, with its one-sample depth as a texture (set 1, the late pass's
// SoftParticlePass layout). For each pixel near the camera: a short ray toward
// the sun through the depth; where something stands in the way, the pixel's
// colour is multiplied by what is left of its light once the sun's share is
// taken away (colourFactor), blended dst * src. Pixels it leaves alone are
// discarded, so they cost a depth read.

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
    vec4 target;       // xy the render extent, z darkness (Light.dbc x setting x outdoors), w debug
} pc;

layout(location = 0) out vec4 outColor;

const float kRayYards = 0.6;
const int kSteps = 8;          // along the ray, shared by the 2x2 quad
const int kStepsPerPixel = 2;  // each pixel takes two of them
const float kMaxDistance = 24.0;
const float kFadeYards = 8.0;
const float kThicknessYards = 0.35;
const float kThicknessPerYard = 0.01;
const float kEdgeYards = 0.15;
const float kEdgePerYard = 0.03;
const float kQuadDepthYards = 0.1;
const float kQuadDepthPerYard = 0.02;

float eyeDistance(float d) {
    return projection[3][2] / (d + projection[2][2]);
}

float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// The value `v` holds in the other pixel of this one's horizontal (or
// vertical) pair of the 2x2 quad, from the fine derivative: the quad's four
// pixels run together, and dFdxFine is the right pixel's value less the
// left's. Every pixel of the quad must reach these calls (no discard or
// divergent branch before them).
float otherH(float v, bool odd) { float d = dFdxFine(v); return odd ? v - d : v + d; }
float otherV(float v, bool odd) { float d = dFdyFine(v); return odd ? v - d : v + d; }

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float d = texelFetch(uSceneDepth, px, 0).r;
    vec2 uv = gl_FragCoord.xy / pc.target.xy;
    vec4 w = pc.invViewProj * vec4(uv * 2.0 - 1.0, d, 1.0);
    vec3 pos = w.xyz / w.w;
    // The surface's facing from the depth's own slope (one quad's worth),
    // taken before any pixel of the quad leaves.
    vec3 n = cross(dFdx(pos), dFdy(pos));
    float dd = eyeDistance(d);
    float slope = max(abs(dFdx(dd)), abs(dFdy(dd)));

    float dist = dd;
    float nl2 = dot(n, n);
    vec3 ldir = normalize(-lightDir.xyz);
    n *= inversesqrt(max(nl2, 1e-24));
    vec3 toCam = viewPos.xyz - pos;
    if (dot(n, toCam) < 0.0) n = -n;
    float ndotl = dot(n, ldir);
    // Sky, past the reach, a silhouette (the quad spans two surfaces and its
    // facing is neither's), or turned from the sun: nothing to take away.
    // Such a pixel still runs to the quad exchange below, marching nothing.
    bool marching = d < 1.0 && dist > 0.0 && dist <= kMaxDistance &&
                  slope <= kEdgeYards + kEdgePerYard * dist && nl2 >= 1e-12 && ndotl > 0.05;

    // The march is shared by the quad: pixel q of the four (by its position
    // in the 2x2) takes steps q and q + 4 of eight, and the four then pool
    // what they found - a quarter of the depth reads of every pixel marching
    // all its own, with each pixel's own depth still deciding whether its
    // neighbours' finding counts (a neighbour on another surface is not
    // taken). Earlier steps weigh more, so the pooled value is the largest.
    bool oddX = (px.x & 1) != 0, oddY = (px.y & 1) != 0;
    int lane = (oddX ? 1 : 0) + (oddY ? 2 : 0);
    float occ = 0.0;
    if (marching) {
        float thick = kThicknessYards + kThicknessPerYard * dist;
        vec3 start = pos + n * (0.02 + 0.002 * dist);
        float jitter = ign(floor(gl_FragCoord.xy * 0.5));  // one per quad: its lanes interleave
        // The ray's ends in clip space: a point along a line is the same
        // blend of its ends' clip coordinates, so each step is a mix and a
        // divide, not a matrix.
        vec4 c0 = projection * (view * vec4(start, 1.0));
        vec4 c1 = projection * (view * vec4(start + ldir * kRayYards, 1.0));
        for (int k = 0; k < kStepsPerPixel; ++k) {
            int i = lane + 4 * k;
            float t = (float(i) + 0.5 + 0.5 * jitter) / float(kSteps);
            vec4 c = mix(c0, c1, min(t, 1.0));
            if (c.w <= 0.0) break;
            vec2 suv = c.xy / c.w * 0.5 + 0.5;
            if (suv.x < 0.0 || suv.y < 0.0 || suv.x >= 1.0 || suv.y >= 1.0) break;
            float sd = texelFetch(uSceneDepth, ivec2(suv * pc.target.xy), 0).r;
            float behind = c.w - eyeDistance(sd);
            if (behind > 0.02 && behind < thick) {
                float ti = float(i) / float(kSteps);
                occ = 1.0 - ti * ti;
                break;
            }
        }
    }
    // Pool across the quad, horizontally then vertically (which reaches the
    // diagonal too). An inactive pixel reports nothing; a neighbour further
    // off in depth than a few inches is another surface and is not taken.
    float tol = kQuadDepthYards + kQuadDepthPerYard * dist;
    float ddA = marching ? dd : -1e6;
    float oh = otherH(occ, oddX), dh = otherH(ddA, oddX);
    float occH = max(occ, abs(dh - dd) < tol ? oh : 0.0);
    float ov = otherV(occH, oddY), dv = otherV(ddA, oddY);
    float occQ = max(occH, abs(dv - dd) < tol ? ov : 0.0);
    occ = occQ;
    if (!marching || occ <= 0.0) discard;

    float fadeT = clamp((kMaxDistance - dist) / kFadeYards, 0.0, 1.0);
    occ *= fadeT * fadeT * (3.0 - 2.0 * fadeT);

    if (pc.target.w > 0.5) {
        // WOWEE_CONTACT_SHADOW_DEBUG: what the ray finds, shaded or not.
        outColor = vec4(1.0, 1.0 - 0.8 * occ, 1.0 - 0.8 * occ, 1.0);
        return;
    }
    float darkness = clamp(pc.target.z, 0.0, 1.0);
    float mapLit = 1.0;
    if (shadowParams.x > 0.5) mapLit = sunShadow(pos, n, ldir);
    float litNew = min(mapLit, 1.0 - occ);
    if (litNew >= mapLit) discard;
    float s = 1.0 - darkness * (1.0 - mapLit);
    float sNew = 1.0 - darkness * (1.0 - litNew);
    float amb = lum(ambientColor.rgb);
    float dif = lum(lightColor.rgb) * ndotl;
    float before = amb + dif * s;
    if (before <= 1e-4) discard;
    float f = clamp((amb + dif * sNew) / before, 0.0, 1.0);
    outColor = vec4(f, f, f, 1.0);
}
