#version 450

// The canopy shafts, a pixel at a time (docs/re/enhanced-lighting.md 5c,
// "Crisp beams"). Drawn by ContactShadowPass with this shader, added to the
// scene (src.rgb + dst) once the opaque scene is done.
//
// The mist's cells are a pixel tile across - twenty pixels of a wide view -
// and each is blended with nine tenths of its last frame, so a beam built in
// them is a soft blob however sharp the shadow that makes it. Here the cells
// keep only what is smooth about a beam: where the gaps in the canopy are and
// how much the air there would scatter (the inject's envelope, set 2). The
// edge comes from the shadow maps themselves - the detail map's 0.07 yards a
// texel near the player, the world map past it - read along each pixel's own
// ray, so a beam ends where the leaf's shadow does, to the pixel.
//
// A column of cells with no beam in it costs one read (the span the
// integrate pass leaves in the first slice); the others spend their steps
// only between the first and last cell that hold one.
//
// Where the steps fall is what keeps the beams free of grain. A shadow is the
// same all along the light's direction, so what a ray meets depends only on
// how far it has moved across the light - its distance from the line through
// the camera along the light. The steps are taken at fixed values of that
// distance, the same for every pixel: two pixels in line with the light on
// screen then read the same points of the shadows, and what the steps miss
// between them shows as fine rays along the light, as leaves do cast, not as
// a speckle. The spacing doubles as a ray crosses more of the light, eased
// from one spacing to the next, so no pixel takes more than kSteps; a ray
// nearly along the light crosses little and needs few. And since neighbours
// read the same points, the four pixels of a 2x2 quad share them: each takes
// every fourth step and they pool what they found, as the contact shadows do.

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
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    ivec4 localLightTileInfo;
    vec4 localLightTileScale;
    uvec4 localLightTiles[288];
    mat4 nearLightSpaceMatrix;
    vec4 nearShadowParams;
    uvec4 nearShadowMask[32];
    mat4 detailLightSpaceMatrix;
    vec4 detailShadowParams;  // x = on, y = one texel in uv, z = 1 / depth range (yd), w = yards a texel
};

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 6) uniform sampler2DShadow uDetailShadowMap;

layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

// The mist's beam envelope (VolumetricFog::beamSet): r what a yard of the
// cell's air scatters as part of a beam where the light reaches it; in the
// first slice, g and b the slice coordinates the column's beams lie between
// (b 0: none).
layout(set = 2, binding = 0) uniform sampler3D uBeam;

layout(push_constant) uniform Push {
    mat4 invViewProj;  // clip -> world, this frame's
    vec4 target;       // xy the render extent, z strength, w flags (4 debug: the spans)
    vec4 extra;        // x how far it is night (the moonbeams keep more of their blue)
} pc;

layout(location = 0) out vec4 outColor;

const float kSteps = 16.0;       // the most a pixel takes
const float kFinest = 0.02;      // yards across the light: the closest two steps come
const float kStartYards = 3.0;   // the inject's beams begin there
const float kQuadYards = 0.05;    // how far two pixels' marches may end apart and still pool:
const float kQuadShare = 0.1;     // this, and this share of the stretch marched
// The mist's own ceiling (volumetric_fog_integrate.comp): the beams ease
// onto it rather than climbing to white along a view down a beam.
const float kMistCeiling = 1.0;
const float kMistCeilingSun = 0.35;

float eyeDistance(float d) {
    return projection[3][2] / (d + projection[2][2]);
}

// The value `v` holds in the other pixel of this one's horizontal (or
// vertical) pair of the 2x2 quad (contact_shadow.frag): every pixel of the
// quad must reach these calls.
float otherH(float v, bool odd) { float d = dFdxFine(v); return odd ? v - d : v + d; }
float otherV(float v, bool odd) { float d = dFdyFine(v); return odd ? v - d : v + d; }

float sliceDepth(float s) {
    return volumetricParams.y * exp(s / (volumetricParams.w * volumetricParams.z));
}

// The inject's two reads (volumetric_fog_inject.comp sunVisibility), through the
// compare samplers, so with
// the hardware's 2x2 compare on the world map as well.
float beamWorldVisibility(vec3 p) {
    vec4 ls = lightSpaceMatrix * vec4(p, 1.0);
    vec3 proj = ls.xyz / ls.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || proj.z < 0.0 || proj.z > 1.0) return 1.0;
    float lit = textureLod(uShadowMap, vec3(uv, proj.z - 0.0005), 0.0);
    vec2 edge = abs(proj.xy);
    return mix(lit, 1.0, smoothstep(0.8, 1.0, max(edge.x, edge.y)));
}

float beamVisibility(vec3 p) {
    if (shadowParams.x < 0.5) return 1.0;
    if (detailShadowParams.x < 0.5) return beamWorldVisibility(p);
    vec4 ls = detailLightSpaceMatrix * vec4(p, 1.0);
    vec2 uv = ls.xy * 0.5 + 0.5;
    float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
    if (edge <= 0.01 || ls.z < 0.0 || ls.z > 1.0) return beamWorldVisibility(p);
    float w = smoothstep(0.01, 0.09, edge);
    float z = ls.z - 2.0 * detailShadowParams.w * detailShadowParams.z;
    float detail = textureLod(uDetailShadowMap, vec3(uv, z), 0.0);
    return w >= 1.0 ? detail : mix(beamWorldVisibility(p), detail, w);
}

vec3 softCeiling(vec3 light, vec3 ceiling) {
    vec3 knee = 0.5 * ceiling;
    vec3 over = max(light - knee, vec3(0.0));
    vec3 room = ceiling - knee;
    return min(light, knee) + room * (1.0 - exp(-over / room));
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float d = texelFetch(uSceneDepth, px, 0).r;
    vec2 uv = gl_FragCoord.xy / pc.target.xy;
    // The pixel's ray, a yard of view depth long, as the cells are placed.
    vec4 w = pc.invViewProj * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    vec3 dir = normalize(w.xyz / w.w - viewPos.xyz);
    vec3 forward = -vec3(view[0][2], view[1][2], view[2][2]);
    vec3 ray = dir / max(dot(dir, forward), 1e-4);
    float sceneDepth = d < 1.0 ? eyeDistance(d) : 1e9;

    // The stretch of this column that holds a beam: the four columns a
    // filtered read of the envelope here takes.
    ivec3 size = textureSize(uBeam, 0);
    vec2 at = uv * vec2(size.xy) - 0.5;
    ivec2 c0 = ivec2(floor(at));
    float first = 1e9, last = 0.0;
    // One filtered read of the four says whether any holds a beam.
    bool any4 = textureLod(uBeam, vec3(uv, 0.5 / float(size.z)), 0.0).b > 0.0;
    for (int i = 0; any4 && i < 4; ++i) {
        ivec2 c = clamp(c0 + ivec2(i & 1, i >> 1), ivec2(0), size.xy - 1);
        vec2 span = texelFetch(uBeam, ivec3(c, 0), 0).gb;
        if (span.y > 0.0) {
            first = min(first, span.x);
            last = max(last, span.y);
        }
    }
    float from = max(sliceDepth(first), kStartYards);
    float to = last > 0.0 ? min(sliceDepth(last), sceneDepth) : 0.0;
    bool marching = to > from;

    bool oddX = (px.x & 1) != 0, oddY = (px.y & 1) != 0;
    int lane = (oddX ? 1 : 0) + (oddY ? 2 : 0);
    float found = 0.0;
    bool whole = false;   // one point stood for the whole stretch: nothing to pool
    if (marching) {
        // How fast the ray moves across the light, per yard of view depth.
        vec3 along = normalize(lightDir.xyz);
        float across = length(ray - along * dot(ray, along));
        float v0 = from * across, v1 = to * across;
        // The spacing: kFinest doubled until the stretch takes kSteps or
        // fewer, and how far it is on the way to the next doubling.
        float level = max(log2(max(v1 - v0, 1e-6) / (kSteps * kFinest)), 0.0);
        float spacing = kFinest * exp2(ceil(level));
        float ease = 1.0 - (ceil(level) - level);   // 0 just after a doubling, 1 at the next
        if (v1 - v0 < kFinest) {
            // Along the light: one point stands for the whole stretch.
            float depth = 0.5 * (from + to);
            float slice = log(depth / volumetricParams.y) * volumetricParams.z;
            float envelope = textureLod(uBeam, vec3(uv, slice), 0.0).r;
            if (envelope > 0.0) found = envelope * beamVisibility(viewPos.xyz + ray * depth) * (to - from);
            whole = true;
        } else {
            // Of the steps, this pixel takes every fourth - its place in the
            // 2x2 quad says which - and the four pool what they found below.
            float n = floor(v0 / spacing);
            n += mod(float(lane) - n, 4.0);
            for (int i = 0; i < (int(kSteps) + 3) / 4 + 1; ++i, n += 4.0) {
                float v = n * spacing;
                // Each step stands for the stretch round it; the odd ones
                // give theirs up to the even as the next doubling nears, and
                // the first and last only for what lies inside the march.
                bool even = mod(n, 2.0) < 0.5;
                float half_ = 0.5 * spacing * (even ? 1.0 + ease : 1.0 - ease);
                if (v - half_ >= v1) break;
                float covered = min(v + half_, v1) - max(v - half_, v0);
                if (covered <= 0.0) continue;
                float depth = clamp(v / across, from, to);
                float slice = log(depth / volumetricParams.y) * volumetricParams.z;
                float envelope = textureLod(uBeam, vec3(uv, slice), 0.0).r;
                if (envelope <= 0.0) continue;
                found += envelope * beamVisibility(viewPos.xyz + ray * depth) * covered / across;
            }
        }
        // Per yard of view depth so far: the ray's own length.
        found *= length(ray);
    }
    // The quad's four pixels took a quarter of the steps each: together
    // they have them all. A neighbour whose march ended elsewhere - its ray
    // stops on a nearer surface, inside the beams - marched another stretch;
    // its steps are not taken, and the pixel's own stand in for them. What
    // lies behind the beams' far end does not matter: over a background of
    // leaves every quad would differ, and each pixel left on its own steps
    // was a speck.
    float reach = marching ? to : 0.0;
    float tol = kQuadYards + kQuadShare * max(to - from, 0.0);
    float part = whole ? 0.25 * found : found;
    float ph = otherH(part, oddX), rh = otherH(reach, oddX);
    float pairH = part + (abs(rh - reach) < tol ? ph : part);
    float pv = otherV(pairH, oddY), rv = otherV(reach, oddY);
    found = pairH + (abs(rv - reach) < tol ? pv : pairH);
    if (found <= 1e-5) discard;

    // A beam is the light's hue thinned toward white, warm by day; the
    // moonbeams keep more of the night's blue (the inject's own colour).
    float thin = mix(0.35, 0.6, 1.0 - pc.extra.x);
    vec3 beam = mix(lightColor.rgb, vec3(dot(lightColor.rgb, vec3(0.2126, 0.7152, 0.0722))), thin);
    vec3 light = beam * (found * pc.target.z);
    if ((localLightMeta.z & 128) != 0) {
        light = softCeiling(light, max(kMistCeiling * fogColor.rgb + kMistCeilingSun * lightColor.rgb, vec3(0.02)));
    }
    if ((int(pc.target.w) & 4) != 0) light = vec3(marching ? 0.2 : 0.0, found * 4.0, 0.0);
    outColor = vec4(light, 1.0);
}
