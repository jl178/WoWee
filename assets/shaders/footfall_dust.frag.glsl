#version 450

// "Dust kicked up" (FootfallDust): a soft, slightly uneven round puff,
// fogged as the world is and alpha-blended.

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

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColourAlpha;
layout(location = 2) in float vFog;
layout(location = 3) in float vSeed;

layout(location = 0) out vec4 outColor;

float dustHash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float dustClumpNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = dustHash(i);
    float b = dustHash(i + vec2(1.0, 0.0));
    float c = dustHash(i + vec2(0.0, 1.0));
    float d = dustHash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec2 p = vUV * 2.0 - 1.0;
    float r2 = dot(p, p);
    if (r2 >= 1.0) discard;
    float shape = (1.0 - r2) * (1.0 - r2);
    // Clumps, so a puff is not a perfect disc; fixed per sprite.
    float n = dustClumpNoise(vUV * 3.0 + vec2(vSeed, vSeed * 0.37));
    float a = vColourAlpha.a * shape * (0.65 + 0.7 * n);
    if (a < 0.003) discard;
    vec3 c = mix(fogColor.rgb, vColourAlpha.rgb, vFog);
    outColor = vec4(c, a);
}
