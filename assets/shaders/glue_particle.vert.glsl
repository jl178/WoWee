#version 450

// Glue scene particles (GlueSceneParticles), drawn into the character
// preview's off-screen pass. Self-contained on purpose: no per-frame set, no
// fog volume - everything it needs is in the push block - so nothing the
// world's descriptor layouts or the preview's per-frame set carry can reach
// this draw. The quads are built on the CPU (m2_particle_stock.hpp), so this
// only places them.

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 unused0;
    vec4 unused1;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main() {
    vColor = clamp(aColor, vec4(0.0), vec4(1.0));
    vUV = aUV;
    gl_Position = push.viewProj * vec4(aPos, 1.0);
}
