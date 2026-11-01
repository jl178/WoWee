#version 450

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 color;
    vec4 center;
    vec4 facing;
} push;

layout(set = 0, binding = 0) uniform sampler2D uSelectTex;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vHeight;

layout(location = 0) out vec4 outColor;

// The stock "ShadowAdd" ramp (Wow.exe 0x7E36E0), sampled by height through the
// box: rises over the bottom sixth, flat, falls over the top sixth.
float heightRamp(float t) {
    if (t <= 0.0 || t >= 1.0) return 0.0;
    float k = t * 12.0;
    if (k < 2.0) return k * 0.5;
    if (k < 10.0) return 1.0;
    return max(0.0, (12.0 - k) * 0.5);
}

void main() {
    if (vUV.x < 0.0 || vUV.y < 0.0 || vUV.x > 1.0 || vUV.y > 1.0) discard;
    vec4 tex = texture(uSelectTex, vUV);
    float alpha = tex.a * heightRamp(vHeight) * push.color.a;
    if (alpha < 0.004) discard;
    // Blended additively (GxBlend_Add): src * srcAlpha + dst.
    outColor = vec4(push.color.rgb * tex.rgb, alpha);
}
