#version 450

// The stock underwater particulates (0x79ca70): camera-facing quads of the
// WaterPoop02 atlas, built on the CPU in world space (stock_particulates.hpp).

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

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vWorldPos;

void main() {
    vUV = aUV;
    vWorldPos = aPos;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
