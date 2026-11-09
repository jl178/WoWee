#version 450

// The stock horizon (TerrainHorizon, 0x795f80): the map's WDL heights.
// Built again with HORIZON_LIT (terrain_horizon_lit.vert.spv) for the lit
// horizon (E11), which also passes each vertex's normal on.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};

layout(location = 0) in vec3 aPosition;
#ifdef HORIZON_LIT
layout(location = 1) in vec4 aNormal;
layout(location = 2) out vec3 vNormal;
#endif

layout(location = 0) out float vViewDepth;
layout(location = 1) out vec3 vWorldPos;

void main() {
    vec4 v = view * vec4(aPosition, 1.0);
    vViewDepth = -v.z;
    vWorldPos = aPosition;
#ifdef HORIZON_LIT
    vNormal = aNormal.xyz;
#endif
#ifdef HORIZON_COLOR
    // The far field a little further along its view ray (far_field::
    // kDepthPushBack): the same pixel, but behind the real ground where the
    // two overlap at the far clip.
    gl_Position = projection * vec4(v.xyz * 1.004, 1.0);
#else
    gl_Position = projection * v;
#endif
}
