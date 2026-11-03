#version 450

// The stock horizon (TerrainHorizon, 0x795f80): the map's WDL heights.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};

layout(location = 0) in vec3 aPosition;

layout(location = 0) out float vViewDepth;
layout(location = 1) out vec3 vWorldPos;

void main() {
    vec4 v = view * vec4(aPosition, 1.0);
    vViewDepth = -v.z;
    vWorldPos = aPosition;
    gl_Position = projection * v;
}
