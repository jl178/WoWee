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
const int kSteps = 6;
const float kMaxDistance = 35.0;
const float kFadeYards = 12.0;
const float kThicknessYards = 0.35;
const float kThicknessPerYard = 0.01;
const float kEdgeYards = 0.15;
const float kEdgePerYard = 0.03;

float eyeDistance(float d) {
    return projection[3][2] / (d + projection[2][2]);
}

float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float lum(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

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

    if (d >= 1.0) discard;  // sky
    float dist = dd;
    if (dist > kMaxDistance || dist <= 0.0) discard;
    // A silhouette: the quad spans two surfaces and its facing is neither's.
    if (slope > kEdgeYards + kEdgePerYard * dist) discard;
    float nl2 = dot(n, n);
    if (nl2 < 1e-12) discard;
    n *= inversesqrt(nl2);
    vec3 toCam = viewPos.xyz - pos;
    if (dot(n, toCam) < 0.0) n = -n;
    vec3 ldir = normalize(-lightDir.xyz);
    float ndotl = dot(n, ldir);
    if (ndotl <= 0.05) discard;  // turned from the sun: no sun to take away

    float thick = kThicknessYards + kThicknessPerYard * dist;
    vec3 start = pos + n * (0.02 + 0.002 * dist);
    float jitter = ign(gl_FragCoord.xy);
    float occ = 0.0;
    // The ray's ends in clip space: a point along a line is the same blend of
    // its ends' clip coordinates, so each step is a mix and a divide, not a
    // matrix.
    vec4 c0 = projection * (view * vec4(start, 1.0));
    vec4 c1 = projection * (view * vec4(start + ldir * kRayYards, 1.0));
    for (int i = 0; i < kSteps; ++i) {
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
    if (occ <= 0.0) discard;

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
