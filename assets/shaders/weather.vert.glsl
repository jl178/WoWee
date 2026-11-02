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
    // Not the scene's fog: both rain draws (0x78A640, 0x78A030) set
    // rain.bls's c0 to (-0.2, 15, 1) and it takes
    // fogcoord = clamp(15 - 0.2 * (z - camera z)), the height above the eye,
    // so a drop fades to the Mod2x grey only from 70 to 75 yards up - never
    // inside the box the rain falls in. Snow writes no fog (snowpoint.bls).
    vFog = clamp(15.0 - 0.2 * (aPos.z - viewPos.z), 0.0, 1.0);
    gl_Position = projection * view * vec4(aPos, 1.0);
}
