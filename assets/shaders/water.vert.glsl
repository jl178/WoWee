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
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
};

layout(push_constant) uniform Push {
    mat4 model;
    // The liquid texture's transform (the stock texture matrix): its scale,
    // and the magma scroll's offset this frame. The fragment stage reads them.
    float texScale;
    float texOffsetU;
    float texOffsetV;
    float liquidBasicType; // 0=water, 1=ocean, 2=magma, 3=slime
    // The rest belongs to the fragment stage, and is spelled the same way here
    // so the two declarations of one range agree.
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float liquidTexValid;
    float shoreFoam;
    float depthRamp;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out float WaveOffset;
layout(location = 4) out vec2 ScreenUV;

// The stock liquid is the flat sheet the ADT and WMO give it: vsLiquidWater
// and vsLiquidMagma move no vertex, and the surface normal is straight up.
// (Gerstner waves used to live here, always at an amplitude of 0; moving the
// surface slid the waterline up and down every character standing in it.)
void main() {
    vec4 worldPos = push.model * vec4(aPos, 1.0);
    WaveOffset = 0.0;
    Normal = vec3(0.0, 0.0, 1.0);
    FragPos = worldPos.xyz;
    TexCoord = aTexCoord;
    vec4 clipPos = projection * view * worldPos;
    gl_Position = clipPos;
    vec2 ndc = clipPos.xy / max(clipPos.w, 1e-5);
    ScreenUV = ndc * 0.5 + 0.5;
}
