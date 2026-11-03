#version 450

// White vertex colour, so the colour and alpha are the texture's own; alpha
// blended. Fogged only for a liquid whose LiquidType Flags carry 0x10 (magma);
// water's motes are drawn unfogged (0x79ca70 sets the fog state from it).

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
};

#include "stock_fog.glsli"

layout(set = 1, binding = 0) uniform sampler2D uMote;

layout(push_constant) uniform Push {
    float fogged;
} push;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vWorldPos;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uMote, vUV);
    // Alpha test at 1/255, as the stock state block sets (alpha ref 1).
    if (tex.a < 1.0 / 255.0) discard;
    vec3 color = tex.rgb;
    if (push.fogged > 0.5) color = mix(fogColor.rgb, color, stockFogVisibility(vWorldPos));
    outColor = vec4(color, tex.a);
}
