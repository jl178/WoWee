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

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in float aAlpha;

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vAlpha;
layout(location = 2) out float vFog;

void main() {
    vUV = aUV;
    vAlpha = aAlpha;
    // The scene's linear fog, as everything else in the world takes it.
    float dist = length(aPos - viewPos.xyz);
    vFog = clamp((fogParams.y - dist) / max(fogParams.y - fogParams.x, 1e-3), 0.0, 1.0);
    gl_Position = projection * view * vec4(aPos, 1.0);
}
