#version 450

// Sunlight under water (E15, not stock; rendering/water_caustics.hpp has the
// why). Drawn on each water and ocean surface just after the water itself,
// with the blend dst * (1 + src): what shows through the surface is brightened
// in proportion to its own colour where the net of focused light crosses the
// bed below. Nothing else is written. The constants match water_caustics.hpp.

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
};

// The world's shadow map (shadow_filter.glsli's binding): one tap is enough
// for a pattern this soft.
layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

// The water's push block (WaterPushConstants), with the strength at its end.
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
    float caustics;   // 0..1: the row on, by the height of the sun
} push;

layout(set = 2, binding = 1) uniform sampler2D SceneDepth;

layout(location = 0) in vec3 FragPos;

layout(location = 0) out vec4 outColor;

const float kMaxGain = 0.3;
const float kDepthIn = 0.15;
const float kDepthPeak = 0.6;
const float kDepthOut = 4.0;
const float kFarStart = 45.0;
const float kFarEnd = 80.0;

float linearizeDepth(float d, float near, float far) {
    return near * far / (far - d * (far - near));
}

vec2 hash22(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return fract((q.xx + q.yz) * q.zy);
}

// Distance from p to the nearest edge of a cell pattern whose points wander
// on small loops: near 0 on an edge, the lines of the net. (Second nearest
// minus nearest point distance, which is close enough to the edge distance
// for lines this soft.)
float cellEdge(vec2 p, float t) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float d1 = 8.0, d2 = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(float(x), float(y));
            vec2 h = hash22(i + o);
            vec2 pt = 0.5 + 0.38 * sin(t * (0.5 + 0.4 * h) + 6.2831853 * h.yx);
            vec2 r = o + pt - f;
            float d = dot(r, r);
            if (d < d1) { d2 = d1; d1 = d; } else if (d < d2) { d2 = d; }
        }
    }
    return sqrt(d2) - sqrt(d1);
}

// The net at a world position (yards): two layers of lines drifting across
// each other, brightest where they cross. Wider and softer deeper down.
float causticNet(vec2 p, float t, float soft) {
    float w = mix(0.10, 0.30, soft);
    float e1 = cellEdge(p * 1.05 + vec2(0.06, 0.035) * t, t * 0.9);
    float e2 = cellEdge(p * 1.55 + vec2(-0.045, 0.07) * t + 7.3, t * 1.15);
    float l1 = 1.0 - smoothstep(0.0, w, e1);
    float l2 = 1.0 - smoothstep(0.0, w * 0.85, e2);
    return clamp(max(l1, l2) * 0.65 + l1 * l2 * 0.35, 0.0, 1.0);
}

void main() {
    if (push.sceneValid < 0.5 || push.caustics <= 0.0) discard;
    // WOWEE_CAUSTICS_DEBUG=1 (strength pushed as 2): red where the surface is
    // drawn, green the depth below it in yards / 4, blue where it is too deep.
    bool dbg = push.caustics > 1.5;

    // Past the fade-out the net is not drawn; skip the depth read.
    if (!dbg && length(FragPos - viewPos.xyz) > kFarEnd) discard;
    float near = push.depthRange.x;
    float far = push.depthRange.y;
    vec2 screenUV = gl_FragCoord.xy / max(push.screenSize, vec2(1.0));
    float waterLin = linearizeDepth(gl_FragCoord.z, near, far);
    float sceneLin = linearizeDepth(texture(SceneDepth, clamp(screenUV, vec2(0.001), vec2(0.999))).r, near, far);
    if (dbg) {
        outColor = vec4(0.5, sceneLin < waterLin - 0.05 ? 1.0 : 0.0, clamp((sceneLin - waterLin) / 8.0, 0.0, 1.0), 0.0);
        return;
    }
    // Something nearer than the surface (a character standing in it, read off
    // the half-size copy along its outline) is not under the water.
    if (sceneLin < waterLin - 0.05) discard;

    // What lies behind the surface on this pixel's ray, and how far below it.
    vec3 toSurface = FragPos - viewPos.xyz;
    vec3 bed = viewPos.xyz + toSurface * (sceneLin / max(waterLin, 1e-4));
    float depth = FragPos.z - bed.z;
    if (depth <= 0.0 || depth >= kDepthOut) discard;

    float dist = length(bed - viewPos.xyz);
    float fade = (1.0 - smoothstep(kFarStart, kFarEnd, dist));
    fade *= (smoothstep(0.0, kDepthIn, depth) * 0.5 + 0.5 * smoothstep(kDepthIn, kDepthPeak, depth)) *
            (1.0 - smoothstep(kDepthPeak, kDepthOut, depth));
    fade *= push.caustics;
    if (fade < 0.004) discard;

    // In the sun's shadow (a pier, a boat, a cliff) there is nothing to focus.
    if (shadowParams.x > 0.5) {
        vec4 ls = lightSpaceMatrix * vec4(bed, 1.0);
        vec3 pr = ls.xyz / ls.w;
        pr.xy = pr.xy * 0.5 + 0.5;
        if (pr.x >= 0.0 && pr.x <= 1.0 && pr.y >= 0.0 && pr.y <= 1.0 && pr.z >= 0.0 && pr.z <= 1.0) {
            fade *= texture(uShadowMap, vec3(pr.xy, pr.z - 0.0015));
        }
        if (fade < 0.004) discard;
    }

    // The net is laid where the light lands on the bed: shifted along the
    // sun's ray by the depth, so it slides as the sun moves, as it does.
    vec3 toSun = normalize(-lightDir.xyz);
    vec2 p = bed.xy + toSun.xy / max(toSun.z, 0.3) * depth;
    float t = fogParams.z;
    float net = causticNet(p, t, smoothstep(0.3, kDepthOut, depth));

    vec3 sun = clamp(lightColor.rgb, vec3(0.0), vec3(1.0));
    outColor = vec4(sun * (kMaxGain * net * fade), 0.0);
}
