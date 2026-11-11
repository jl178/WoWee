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
//
// Built a second time with BOUNCE_GRID (ambient_bounce.frag.spv) for the
// "Bounce light (the reference client)" row: the fill's sky and ground terms then take
// their colour from a grid baked over the ground around the camera
// (rendering/bounce_grid.hpp) - the ground's own colour, the canopy over the
// pixel and its leaves, the walls beside it - in place of one colour
// everywhere. Without the define this file builds exactly as before.

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
#ifdef BOUNCE_GRID
    vec4 extra;        // xy the grid's corner (world), z 1 once it holds a bake
#endif
#ifdef SKY_FIELD
    vec4 sky;          // the sky height field: xy its corner (world), z the
                       // height its depth 0 stands for, w 1 once drawn
#endif
} pc;

#ifdef BOUNCE_GRID
// The bounce grid (bounce_grid.hpp): 256 cells of 2 yards each way.
//   layer 0: ground rgb, canopy cover over the cell
//   layer 1: leaf rgb, wall presence
//   layer 2: canopy foot z, canopy top z, wall foot z, wall top z
layout(set = 2, binding = 0) uniform sampler2DArray uBounceGrid;
const float kGridSpan = 512.0;         // bounce_grid::kSpan
// The grid's outer band fades to the plain fill, so its edge (the camera
// is at most kRecentreYards from the middle) never shows as a line.
const float kGridEdge = 0.1;
// Under a full canopy: the sky's fill is mostly gone (the leaves take it),
// the ground below is in their shade (its bounce weaker), and the ambient
// itself - the light from all round - is dimmer and is the sky's light
// passed through the leaves: half the sky's hue, half the leaves'. Against
// Jered's live Enhanced captures that is what its canopy shade is: darker
// than 3.3.5's, and a cool olive-green where 3.3.5's is golden (the leaf
// textures are yellow-green; the light under them is not).
const float kCanopySky = 0.7;
const float kLeafFill = 0.15;
const float kCanopyGroundShade = 0.7;
const float kCanopyAmbient = 0.9;
const float kCanopyTint = 1.0;
// A wall beside the pixel, its face toward the pixel in the sun, sends back
// that sun in a warm stone-and-timber colour (most of 3.3.5's walls), to a
// face turned toward it. Looked for kWallReach yards out on four sides.
const float kWallReach = 3.0;
const float kWallFill = 0.25;
const vec3 kWallAlbedo = vec3(0.5, 0.42, 0.32);
#endif

#ifdef SKY_FIELD
// Built a fourth and fifth time with BOUNCE_GRID and SKY_FIELD
// (ambient_sky.frag.spv, ambient_sky_fill.frag.spv) for the "Sky shade (WoW
// Enhanced)" row.
// The sky over each point under Enhanced's daylight (sky_height_field.hpp;
// docs/re/enhanced-lighting.md 6e): depth maps from straight above round the
// camera - the tops of the buildings and doodads, the terrain alone - and
// from straight below, the undersides of the buildings and doodads.
layout(set = 3, binding = 0) uniform sampler2D uSkyTops;
layout(set = 3, binding = 1) uniform sampler2D uSkyGround;
layout(set = 3, binding = 2) uniform sampler2D uSkyFeet;
const float kSkySpan = 256.0;          // sky_field::kSpan
const float kSkyDepth = 400.0;         // sky_field::kDepth
// The horizon is looked for along kSkyDirs directions, at these reaches
// (yards): far enough to see past the crown of an oak from under it.
const int kSkyDirs = 8;
const int kSkyReaches = 3;
const float kSkyReach[kSkyReaches] = float[](1.5, 5.0, 12.0);
// Height a sample must stand above the point to count (the field's own
// half-yard texels and the depth's slope bias).
const float kSkyBias = 0.35;
// How much of the sky behind it a slab takes: all of it - the gaps between
// a crown's leaves are already holes in the maps (alpha-tested).
const float kSkyDensity = 1.0;
// The quad pools its looks across depths this close (yards, and per yard).
const float kSkyQuadYards = 0.3;
const float kSkyQuadPerYard = 0.05;
// Under a closed sky the ground's ambient keeps this much of itself (the
// light from all round that comes in under the crowns), a model's
// kModelClosed (a trunk or a wall stands in the open air beside it).
const float kSkyClosed = 0.7;
const float kModelClosed = 0.9;
// Enhanced's probe GI (giQuality 3) stands in as a fill with no facing - its
// irradiance volume is read at the pixel and added (section 6) - on the
// ground in shade. Only in the open: none at kOpenFrom of the sky seen,
// all of it from kOpenTo.
const float kOpenFrom = 0.3;
const float kOpenTo = 0.9;
const float kGiSky = 0.2;            // x the fog colour (the sky's horizon)
const float kGiBounce = 0.6;         // x the sun x the ground's colour
const float kGiBounceShaded = 0.8;   // of it with none of the ground round lit
const float kGiSunRef = 0.6;         // sin of the sun's height it was set at
const float kGiSunPow = 2.0;
const float kGiBounceSat = 0.6;      // of the ground's chroma it keeps
const float kGiCool = 0.35;          // of it in the sky's hue
// The ground's lit share is read on two rings round the point (yards).
const float kGiRing0 = 3.0;
const float kGiRing1 = 8.0;
// How far the shade may be lifted (the plain fill: kFillMaxLift).
const float kEnhancedMaxLift = 2.5;
// Shade is where the sun gives under kShadeFrom of the light, none past
// kShadeTo: the fill is for shade.
const float kShadeFrom = 0.05;
const float kShadeTo = 0.2;
#endif

layout(location = 0) out vec4 outColor;

// SSAOSettings 116 (Enhanced 1.60.1.70862).
const float kRadius = 0.6;           // yards (Enhanced's 1 drew rims a hand wide)
// The most the disk spans on screen, as a share of its height (see main).
const float kMaxRadiusScreen = 0.03;
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
// Jered on the live client (Nov 2026): "too strong - it just creates
// shadows around things". At most a fifth of the ambient now (it was 0.4),
// a contact shade rather than a dark rim round every object.
const float kFloor = 0.8;
// Where it fades out. ASSAO's defaults are 50 to 300 yards; past 120 a
// 1-yard disk is under two pixels at 1080 lines, so it stops there.
const float kFadeFrom = 60.0;
// Fill light: how much of the horizon colour the sky gives an upward face,
// and how much of the sun the ground sends back up at a typical Elwynn grass
// and dirt colour. Tuned side by side against Enhanced's shade in Goldshire.
const float kFillSky = 0.2;
const float kFillBounce = 0.2;
// The most the fill may lift a pixel's light, and only in sunlight: shade
// has to stay clearly darker than what the sun reaches, or the picture
// goes flat and grey (Jered's Goldshire evening, round 2).
const float kFillMaxLift = 1.15;
const vec3 kGroundAlbedo = vec3(0.45, 0.42, 0.28);
const float kFadeTo = 120.0;
const int kTaps = 6;
const float kPlaneBias = 0.03;        // yards
const float kPlaneBiasPerYard = 0.003;
const float kQuadDepthYards = 0.1;
const float kQuadDepthPerYard = 0.02;
// Depth edges (ASSAO computes the same edges for its blur). Where the
// surface breaks - a trunk's or an arm's silhouette, the cards of a leaf
// cluster - the facing rebuilt from depth is a guess, and the disk then
// counts whatever stands behind or beside as cover: the dark grey jagged
// lines along every branch and leaf edge in Jered's Goldshire. A pixel whose
// neighbours do not continue its surface (the second difference of the
// positions, yards) takes no occlusion; a smooth surface is untouched.
const float kEdgeYards = 0.08;
const float kEdgePerYard = 0.012;
// Thickness: a tap well in front of or behind the pixel along the view is
// another object - a leaf card, a branch, an arm across the body - not a
// crease the pixel sits in. Such a tap counts less from kThickNear of the
// disk's radius in depth difference and not at all from kThickFar.
const float kThickNear = 0.35;
const float kThickFar = 0.8;

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

#ifdef SKY_FIELD
// The highest top of the four texels round uv, as a world height (far
// below the ground where nothing stands).
float skyTop(vec2 uv) {
    vec4 d = textureGather(uSkyTops, uv, 0);
    return pc.sky.z - kSkyDepth * min(min(d.x, d.y), min(d.z, d.w));
}

// The lowest underside of the four (far above where nothing stands).
float skyFoot(vec2 uv) {
    vec4 d = textureGather(uSkyFeet, uv, 0);
    return pc.sky.z - kSkyDepth + kSkyDepth * min(min(d.x, d.y), min(d.z, d.w));
}

// sin^2 of the elevation whose tangent is t, 0 at and below the horizon: the
// share of a cosine-weighted hemisphere's slice below that elevation.
float sin2Elev(float t) {
    float tt = max(t, 0.0);
    tt *= tt;
    return tt / (1.0 + tt);
}

float skyGroundAt(vec2 uv) {
    return pc.sky.z - kSkyDepth * textureLod(uSkyGround, uv, 0.0).r;
}

// What of the sky a point sees, weighted as an upward face takes it. Along
// each direction, what stands at each reach is a slab from its underside to
// its top, and hides the elevations between them: sin^2 of the top's
// elevation less sin^2 of the underside's is that band's share of the
// slice of a cosine-weighted hemisphere. A crown overhead hides the steep
// sky and leaves the sky under its edge; a wall or a trunk, standing on the
// ground, hides everything up to its top.
//
// The kSkyDirs directions are shared over the 2x2 quad: each pixel looks
// along every fourth (from `lane`), and main pools the quad's, as the
// occlusion pools its spiral - a quarter of the looks a pixel.
float skyVisibilityLane(vec3 p, vec2 uv, int lane) {
    float acc = 0.0;
    for (int i = lane; i < kSkyDirs; i += 4) {
        float a = (float(i) + 0.5) * (6.2831853 / float(kSkyDirs));
        vec2 dir = vec2(cos(a), sin(a)) / kSkySpan;
        float v = 1.0;
        for (int k = 0; k < kSkyReaches; ++k) {
            float r = kSkyReach[k];
            vec2 q = uv + dir * r;
            float hi = sin2Elev((skyTop(q) - p.z - kSkyBias) / r);
            float lo = sin2Elev((skyFoot(q) - p.z) / r);
            v *= 1.0 - kSkyDensity * max(hi - lo, 0.0);
        }
        acc += v;
    }
    return acc / float(kSkyDirs / 4);
}

// How much of the ground round p the sun reaches: one point on each of two
// rings, turned by the pixel's place in its quad (the four of a quad look
// four ways), each set on the ground there and tested against the world
// map once.
float groundSunlit(vec3 p, vec3 ldir, int lane) {
    vec3 row0 = vec3(lightSpaceMatrix[0][0], lightSpaceMatrix[1][0], lightSpaceMatrix[2][0]);
    vec3 row2 = vec3(lightSpaceMatrix[0][2], lightSpaceMatrix[1][2], lightSpaceMatrix[2][2]);
    float texelYd = 2.0 * shadowTexel() / max(length(row0), 1e-9);
    float sinA = sqrt(max(1.0 - ldir.z * ldir.z, 0.0));
    float lift = texelYd * 2.5 * sinA + 0.1;
    float bias = 0.25 * texelYd * length(row2);
    float lit = 0.0;
    for (int i = 0; i < 2; ++i) {
        float r = i == 0 ? kGiRing0 : kGiRing1;
        float a = float(i) * 3.1415927 + float(lane) * 1.5707963;
        vec2 q = p.xy + vec2(cos(a), sin(a)) * r;
        float gz = skyGroundAt((q - pc.sky.xy) / kSkySpan) + lift;
        vec4 ls = lightSpaceMatrix * vec4(q, gz, 1.0);
        vec3 pr = ls.xyz / ls.w;
        pr.xy = pr.xy * 0.5 + 0.5;
        bool inside = all(greaterThanEqual(pr, vec3(0.0))) && all(lessThanEqual(pr, vec3(1.0)));
        lit += inside ? texture(uShadowMap, vec3(pr.xy, pr.z - bias)) : 1.0;
    }
    return lit * 0.5;
}
#endif

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
    // How far the neighbours fail to continue this pixel's surface (see
    // kEdgeYards): 1 on a smooth surface, 0 on a depth edge.
    float edgeTol = kEdgeYards + kEdgePerYard * dist;
    float broken = max(length((pr - posV) - (posV - pl)), length((pu - posV) - (posV - pd)));
    float smoothSurface = 1.0 - smoothstep(edgeTol, 3.0 * edgeTol, broken);

    // The disk's radius in pixels at this depth.
    float radiusPx = kRadius * abs(projection[1][1]) * 0.5 * pc.target.y / max(dist, 1e-3);
    // Near the camera the 1-yard disk spans a hundred pixels or more, and
    // six fixed taps that far apart print copies of every edge they cross
    // as bands beside it - the jagged grey streaks down trunks and arms. The
    // disk is held to kMaxRadiusScreen of the screen's height, the radius in
    // yards shrinking with it, so up close it is contact shading.
    float maxPx = kMaxRadiusScreen * pc.target.y;
    float radiusYd = kRadius * min(1.0, maxPx / max(radiusPx, 1e-3));
    radiusPx = min(radiusPx, maxPx);
    int flags = int(pc.target.w + 0.5);
#ifdef FILL_ONLY
    // Built a third time with FILL_ONLY (ambient_fill.frag.spv) for the fill
    // and bounce light with the occlusion off, the Enhanced preset: the
    // spiral and the quad's pooling then leave the shader, whose size was
    // a tenth of the pass's cost on the Iris Xe. The same picture as the
    // full build with flag 1 clear.
    const bool aoOn = false;
#else
    bool aoOn = (flags & 1) != 0;
#endif
    bool fillOn = (flags & 2) != 0;
#ifdef BOUNCE_GRID
    // The bounce light runs the fill with or without its row.
    bool bounceOn = (flags & 8) != 0 && pc.extra.z > 0.5;
    fillOn = fillOn || (flags & 8) != 0;
#endif
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
            float falloff = max(0.0, 1.0 - len2 / (radiusYd * radiusYd));
            float thick = 1.0 - smoothstep(kThickNear * radiusYd, kThickFar * radiusYd, abs(delta.z));
            sum += max(0.0, ndotd - kHorizon) * falloff * thick;
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
#ifdef SKY_FIELD
    // The sky's visibility under Enhanced's daylight (skyVisibilityLane),
    // pooled over the quad here, before any pixel of it may discard: a
    // neighbour more than a little off in depth is another surface and its
    // looks are not taken. Only inside the field (fading out over its outer
    // band), outdoors, and not on a leaf, which keeps the bounce light as it
    // is (below).
    int skyLane = (oddX ? 1 : 0) + (oddY ? 2 : 0);
    vec2 skyUv = (pos.xy - pc.sky.xy) / kSkySpan;
    float skyInside = 0.0;
    float groundZ = 0.0;
    float above = 0.0;
    if (bounceOn && (localLightMeta.z & 128) != 0 && pc.sky.w > 0.5 && d < 1.0) {
        vec2 se = min(skyUv, 1.0 - skyUv);
        float edge = kSkyReach[kSkyReaches - 1] / kSkySpan;
        skyInside = smoothstep(edge, edge + 0.08, min(se.x, se.y)) * (1.0 - clamp(shadowParams.w, 0.0, 1.0));
        if (skyInside > 0.0) {
            groundZ = skyGroundAt(skyUv);
            above = pos.z - groundZ;
            // A leaf: off the ground, inside something whose underside there
            // (the lower of two looks a yard apart: a pixel on a trunk's side
            // is at the edge of the trunk's own texels) is well above the
            // ground - a crown; not a wall or a trunk, which stand on the
            // ground, nor a unit, which is in none of the maps.
            if (above > 1.0) {
                const float r = 0.75 / kSkySpan;
                vec2 o = (skyLane & 1) != 0 ? vec2(r, r) : vec2(r, -r);
                float foot = min(skyFoot(skyUv + o), skyFoot(skyUv - o));
                float leaf = smoothstep(1.0, 2.5, above) * smoothstep(1.5, 3.0, foot - groundZ) *
                             smoothstep(-1.0, 0.0, skyTop(skyUv) - pos.z) * smoothstep(-1.5, -0.5, pos.z - foot);
                skyInside *= 1.0 - leaf;
            }
        }
    }
    bool skyOn = skyInside > 0.0;
    float visSum = skyOn ? skyVisibilityLane(pos, skyUv, skyLane) : 0.0;
    float visW = skyOn ? 1.0 : 0.0;
    float skyTol = kSkyQuadYards + kSkyQuadPerYard * dist;
    float dS = skyOn ? dist : -1e6;
    float vh = otherH(visSum, oddX), wh = otherH(visW, oddX), dhS = otherH(dS, oddX);
    bool takeSkyH = skyOn && abs(dhS - dist) < skyTol;
    float visH = visSum + (takeSkyH ? vh : 0.0);
    float visWH = visW + (takeSkyH ? wh : 0.0);
    float vv = otherV(visH, oddY), wv2 = otherV(visWH, oddY), dvS = otherV(dS, oddY);
    bool takeSkyV = skyOn && abs(dvS - dist) < skyTol;
    float visQuad = (visH + (takeSkyV ? vv : 0.0)) / max(visWH + (takeSkyV ? wv2 : 0.0), 1.0);
#endif
    if (d >= 1.0 || nl2 < 1e-12) discard;

    float occlusion = 1.0;
    if (working && wQ > 0.0) {
        float obscurance = sumQ / (wQ * float(kTaps));
        float fade = clamp((kFadeTo - dist) / (kFadeTo - kFadeFrom), 0.0, 1.0);
        // Only ground-like receivers: what the occlusion is for is seating
        // grass, roots, crates, walls and feet into the ground. On a
        // character or a tree limb - curved, thin, moving, alpha-tested - the
        // depth-rebuilt facing is a guess from moment to moment, and the
        // occlusion showed as grey patches that came and went (Jered, on the
        // Enhanced preset). A face turned up takes it; one turned sideways or
        // down, as a body, an arm or a branch mostly is, takes none.
        float receiver = smoothstep(0.45, 0.75, n.z);
        obscurance = min(obscurance * kStrength, kClamp) * fade * smoothSurface * receiver;
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
#ifdef BOUNCE_GRID
    vec3 ambPlain = ambNew;
    // How far the sun lights this face: from 0.4 of its light up it keeps
    // the plain fill (below), and the grid is not read at all.
    float sunShare = clamp(lum(dif) / max(lum(amb + dif), 1e-3), 0.0, 1.0);
    float keepPlain = smoothstep(0.05, 0.4, sunShare);
    bool readGrid = bounceOn && keepPlain < 1.0;
#endif
    if (fillOn) {
        // Fill light (a stand-in for Enhanced's probe GI, section 6 of
        // docs/re/enhanced-lighting.md): the sky's light from above, in the
        // zone's horizon colour, and the sun's light bounced off the ground
        // from below, warmed by the ground's colour; a face takes each by how
        // far it turns toward it. Occluded as the ambient is. Outdoors only.
        float sunUp = smoothstep(0.0, 0.1, -normalize(lightDir.xyz).z);
        // On a depth edge the facing is a guess: half sky, half ground.
        float up = mix(0.5, clamp(n.z * 0.5 + 0.5, 0.0, 1.0), smoothSurface);
        vec3 sky = fogColor.rgb * kFillSky;
        vec3 bounce = lightColor.rgb * kGroundAlbedo * (kFillBounce * sunUp);
#ifdef BOUNCE_GRID
        // The plain fill, kept: sunlit faces keep it (below).
        ambPlain = ambNew + (sky * up + bounce * (1.0 - up)) * (pc.target.z * occlusion * (1.0 - indoor) * sunUp);
        vec3 side = vec3(0.0);
        // WOWEE_BOUNCE_DEBUG (flag 16): the grid over the picture - canopy
        // red, walls blue, the ground's hue elsewhere.
        vec3 dbg = vec3(1.0);
        if (readGrid) {
            // What is round this point, from the grid at its world position
            // (bounce_grid.hpp): nothing here depends on the neighbouring
            // pixels, so there is no rim at a silhouette and nothing to
            // flicker as the view or the leaves move.
            vec2 uv = (pos.xy - pc.extra.xy) / kGridSpan;
            vec2 e = min(uv, 1.0 - uv);
            float inGrid = smoothstep(0.0, kGridEdge, min(e.x, e.y));
            if (inGrid > 0.0) {
                vec4 g0 = textureLod(uBounceGrid, vec3(uv, 0.0), 0.0);
                vec4 g1 = textureLod(uBounceGrid, vec3(uv, 1.0), 0.0);
                vec4 g2 = textureLod(uBounceGrid, vec3(uv, 2.0), 0.0);
                // The canopy over this point: all of it below the crowns'
                // foot, none at their top (a sunlit treetop is open sky).
                float under = 1.0 - smoothstep(g2.x, max(g2.y, g2.x + 0.5), pos.z);
                float cover = clamp(g0.a, 0.0, 1.0) * under * inGrid;
                vec3 ground = mix(kGroundAlbedo, g0.rgb, inGrid);
                vec3 leaf = g1.rgb;
                // The light under the leaves: the sky's hue and the
                // leaves', each at unit brightness, met halfway.
                vec3 skyHue = fogColor.rgb / max(lum(fogColor.rgb), 1e-3);
                vec3 leafHue = leaf / max(lum(leaf), 1e-3);
                vec3 underHue = mix(skyHue, leafHue, 0.35);
                underHue /= max(lum(underHue), 1e-3);
                sky = sky * (1.0 - kCanopySky * cover) +
                      lightColor.rgb * underHue * (lum(leaf) * kLeafFill * cover * sunUp);
                float groundLit = 1.0 - kCanopyGroundShade * clamp(g0.a, 0.0, 1.0) * inGrid;
                bounce = lightColor.rgb * ground * (kFillBounce * sunUp * groundLit);
                vec3 canopyAmb = mix(vec3(1.0), underHue, kCanopyTint) * kCanopyAmbient;
                ambNew *= mix(vec3(1.0), canopyAmb, cover * (1.0 - indoor) * mix(0.5, 1.0, sunUp));
                // Walls: four looks kWallReach yards out. Each counts by
                // how much wall stands there at this height, how far this
                // face turns toward it, and how far the wall's face toward
                // this point turns to the sun.
                vec2 toSun = -lightDir.xy;
                float toSunLen = length(toSun);
                toSun = toSunLen > 1e-4 ? toSun / toSunLen : vec2(0.0);
                vec2 nh = n.xy * smoothSurface;
                float wall = 0.0;
                // The wall heights are carried 8 yards past each wall
                // (bounce_grid.hpp): none spanning this height here, none
                // within reach.
                bool wallNear = g2.w > pos.z - 0.5 && g2.z < pos.z + 1.5;
                // Along each axis this face turns toward one side at most
                // (the other's facing is 0, and it adds nothing): one look an
                // axis, toward that side, in the order the four were taken.
                for (int a = 0; a < 2 && wallNear; ++a) {
                    float toward = a == 0 ? nh.x : nh.y;
                    float sgn = toward > 0.0 ? 1.0 : -1.0;
                    vec2 dir = a == 0 ? vec2(sgn, 0.0) : vec2(0.0, sgn);
                    float facing = max(dot(nh, dir), 0.0);
                    float lit = max(dot(-dir, toSun), 0.0);
                    if (facing * lit <= 0.0) continue;
                    vec2 uvk = uv + dir * (kWallReach / kGridSpan);
                    float present = textureLod(uBounceGrid, vec3(uvk, 1.0), 0.0).a;
                    vec2 wz = textureLod(uBounceGrid, vec3(uvk, 2.0), 0.0).zw;
                    float spans = smoothstep(wz.x - 1.5, wz.x - 0.5, pos.z) * smoothstep(pos.z - 0.5, pos.z + 1.5, wz.y);
                    wall += present * spans * facing * lit;
                }
                side = lightColor.rgb * kWallAlbedo * (kWallFill * sunUp * min(wall, 1.0) * inGrid);
                vec3 groundHue = ground / max(lum(ground), 1e-3);
                dbg = mix(vec3(1.0), groundHue, 0.7 * inGrid);
                dbg = mix(dbg, vec3(1.6, 0.3, 0.3), cover);
                dbg = mix(dbg, vec3(0.3, 0.4, 1.8), clamp(g1.a, 0.0, 1.0) * inGrid * 0.8);
                dbg = mix(dbg, vec3(1.8, 1.4, 0.5), min(wall, 1.0));
            }
        }
#endif
#ifdef BOUNCE_GRID
        ambNew += (sky * up + bounce * (1.0 - up) + side) * (pc.target.z * occlusion * (1.0 - indoor) * sunUp);
#else
        ambNew += (sky * up + bounce * (1.0 - up)) * (pc.target.z * occlusion * (1.0 - indoor) * sunUp);
#endif
#ifdef BOUNCE_GRID
        if ((flags & 16) != 0) {
            outColor = vec4(dbg * 0.5, 1.0);
            return;
        }
#endif
    }
    vec3 before = amb + dif;
    // At most half as bright again: the fill lifts shade, it does not light
    // a face that is already lit.
    vec3 f = clamp((ambNew + dif) / max(before, vec3(1e-3)), 0.0, kFillMaxLift);
#ifdef BOUNCE_GRID
    if (bounceOn) {
        // The bounce light changes shade only. A face the sun reaches keeps
        // what the plain fill gave it - not brighter, and not darker for a
        // canopy's disc that overhangs a sunlit street - fading over the
        // first part of the sun's share of its light.
        vec3 fPlain = clamp((ambPlain + dif) / max(before, vec3(1e-3)), 0.0, kFillMaxLift);
        f = mix(f, fPlain, keepPlain);
    }
#endif
#ifdef SKY_FIELD
    // Enhanced's daylight with the sky height field drawn (docs/re/
    // enhanced-lighting.md 6e). What the light at the pixel was is taken
    // apart again - the ambient by Enhanced's ramp toward the sun (held to
    // flat ground's on terrain), the sun under its shadow (all of it on
    // terrain, terrain.frag; 1 - shadowParams.y of it on models) - and put
    // back with the ambient scaled by what of the sky the point sees:
    // - the ground in shade takes a fill for Enhanced's probe GI: the sky's
    //   light, and the sun off the ground round it - more where more of that
    //   ground is in the sun - partly in the sky's hue, as Enhanced's shade
    //   keeps it; under the trees (little open sky) none of it;
    // - a model (a trunk, a wall, a unit) keeps the bounce light above, its
    //   ambient scaled by its sky;
    // - a leaf keeps the bounce light above as it is: the crowns' colour was
    //   matched with it, and the warm bounce stays out of them.
    if (skyInside > 0.0) {
        float onGround = (1.0 - smoothstep(0.25, 0.75, above)) * smoothstep(0.5, 0.8, n.z);
        float nl = dot(n, ldir);
        float ramp = mix(0.7, 1.1, 0.5 + 0.5 * clamp(nl, -1.0, 1.0));
        float rampFlat = mix(0.7, 1.1, 0.5 + 0.5 * clamp(ldir.z, -1.0, 1.0));
        ramp = mix(ramp, ramp / rampFlat, onGround);
        float sunKeep = mix(mix(1.0, sunLit, clamp(shadowParams.y, 0.0, 1.0)), sunLit, onGround);
        vec3 ambF = amb * ramp;
        vec3 difF = lightColor.rgb * (max(nl, 0.0) * sunKeep * (1.0 - indoor));
        vec3 beforeF = max(ambF + difF, vec3(1e-3));
        float vis = visQuad;
        float skyK = mix(mix(kModelClosed, kSkyClosed, onGround), 1.0, vis);
        // A model: the bounce light above, less the sky it does not see.
        vec3 fModel = clamp(f + ambF * (occlusion * (skyK - 1.0)) / beforeF, 0.0, kFillMaxLift);
        vec3 fF = fModel;
        if (onGround > 0.0) {
            vec3 gi = vec3(0.0);
            float sunShare = clamp(lum(difF) / lum(beforeF), 0.0, 1.0);
            float shade = 1.0 - smoothstep(kShadeFrom, kShadeTo, sunShare);
            if (shade > 0.0) {
                float sunUpF = smoothstep(0.0, 0.1, -normalize(lightDir.xyz).z);
                vec2 uv = (pos.xy - pc.extra.xy) / kGridSpan;
                vec2 e = min(uv, 1.0 - uv);
                float inGrid = smoothstep(0.0, kGridEdge, min(e.x, e.y));
                vec3 ground = mix(kGroundAlbedo, textureLod(uBounceGrid, vec3(uv, 0.0), 0.0).rgb, inGrid);
                // Light that has crossed yards of ground, grass and
                // trunks is greyer than the ground under the point.
                ground = mix(vec3(lum(ground)), ground, kGiBounceSat);
                // The sun on level ground round it, against its height
                // at 9:22 (37 degrees) where the fill was set against
                // Enhanced's shade; squared, as the light's colour is
                // already scaled to hold level ground's light as the sun
                // sinks (enhanced_palette::sunIrradianceScale).
                float groundSun = pow(clamp(ldir.z, 0.0, 1.0) / kGiSunRef, kGiSunPow);
                vec3 bounce = lightColor.rgb * ground *
                              (kGiBounce * groundSun * mix(kGiBounceShaded, 1.0, groundSunlit(pos, ldir, skyLane)));
                vec3 skyHue = amb / max(lum(amb), 1e-3);
                bounce = mix(bounce, skyHue * lum(bounce), kGiCool);
                float open = smoothstep(kOpenFrom, kOpenTo, vis);
                gi = (fogColor.rgb * kGiSky + bounce) * (open * pc.target.z * occlusion * shade * sunUpF);
            }
            vec3 fGround = clamp((ambF * (occlusion * skyK) + difF + gi) / beforeF, 0.0, kEnhancedMaxLift);
            fF = mix(fModel, fGround, onGround);
        }
        if ((flags & 32) != 0) fF = vec3(vis);
        f = mix(f, fF, skyInside);
    }
#endif
    if (all(lessThan(abs(f - 1.0), vec3(0.002)))) discard;
    outColor = vec4(f * 0.5, 1.0);
}
