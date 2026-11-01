#version 450

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

layout(set = 1, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform Push {
    vec4 tint;   // rgb = colour, a = opacity
} push;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vAlpha;
layout(location = 2) in float vFog;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uSprite, vUV);
    float alpha = tex.a * vAlpha * push.tint.a;
    if (alpha < 0.004) discard;
    // Lit by the scene: rain at night is dark, at noon pale.
    vec3 light = clamp(ambientColor.rgb + lightColor.rgb * 0.6, vec3(0.12), vec3(1.0));
    vec3 color = tex.rgb * push.tint.rgb * light;
    color = mix(fogColor.rgb, color, vFog);
    outColor = vec4(color, alpha);
}
