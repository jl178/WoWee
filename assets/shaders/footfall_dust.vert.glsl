#version 450

// "Dust kicked up" (FootfallDust): camera-facing puffs built on the CPU.

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
};

#include "stock_fog.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColourAlpha;
layout(location = 3) in float aSeed;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColourAlpha;
layout(location = 2) out float vFog;
layout(location = 3) out float vSeed;

void main() {
    vUV = aUV;
    // Lit as a matte, upward-facing thing in the open: the ambient plus part
    // of the sun, so it is dust-coloured at noon and dim at night.
    vec3 light = min(ambientColor.rgb + lightColor.rgb * 0.55, vec3(1.15));
    vColourAlpha = vec4(aColourAlpha.rgb * light, aColourAlpha.a);
    vFog = stockFogVisibility(aPos);
    vSeed = aSeed;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
