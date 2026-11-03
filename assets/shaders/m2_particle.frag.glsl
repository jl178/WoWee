#version 450

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int alphaKey;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;

layout(location = 0) out vec4 outColor;

#ifdef SOFT_PARTICLES
// The late particle pass's build (m2_particle_soft.frag.spv), for the glow
// halos: the scene depth in set 2. The projection for the fade is the
// PerFrame block's, declared here only in this build.
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};
layout(location = 3) in float vSoftRange;
#define SOFT_DEPTH_SET 2
#include "soft_particle.glsli"
#endif

void main() {
    vec2 p = gl_PointCoord;
    float tile = floor(vTile);
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    vec2 uv = (vec2(tx, ty) + p) / push.tileCount;
    vec4 texColor = texture(uTexture, uv);

    if (push.alphaKey != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        if (lum < 0.05) discard;
    }

    // Soft circular falloff for point-sprite edges.
    float edge = 1.0 - smoothstep(0.4, 0.5, length(p - 0.5));
    float alpha = texColor.a * vColor.a * edge * vFogVisibility;
#ifdef SOFT_PARTICLES
    alpha *= softParticleFade(vSoftRange);
#endif
    vec3 rgb = texColor.rgb * vColor.rgb * alpha;
    outColor = vec4(rgb, alpha);
}
