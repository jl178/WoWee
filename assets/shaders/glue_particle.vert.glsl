#version 450

// Glue scene particles (GlueSceneParticles), drawn into the character
// preview's off-screen pass. Self-contained on purpose: no per-frame set, no
// fog volume - everything it needs is in the push block - so nothing the
// world's descriptor layouts or the preview's per-frame set carry can reach
// this draw.

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;   // xyz = camera position, w = pixels per world unit at distance 1
    vec4 limits;      // x = max point size in pixels
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
layout(location = 3) in float aTile;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vTile;

void main() {
    float dist = max(length(aPos - push.cameraPos.xyz), 0.5);
    gl_PointSize = clamp(aSize * push.cameraPos.w / dist, 1.0, push.limits.x);
    vColor = clamp(aColor, vec4(0.0), vec4(1.0));
    vTile = aTile;
    gl_Position = push.viewProj * vec4(aPos, 1.0);
}
