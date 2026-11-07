#version 450

// Smoother characters (Enhancements::characterFidelity): the patch control
// stage for near units. Each edge's level is a function of its two corners
// alone - their view depth and how close to edge-on they are - so the two
// triangles that share an edge always agree on it and no crack opens.
// CharacterRenderer binds this only for units within kTessFar of the camera;
// everything farther is drawn by the stock pipelines.

layout(vertices = 3) out;

layout(location = 0) in vec3 inFragPos[];
layout(location = 1) in vec3 inNormal[];
layout(location = 2) in vec2 inTexCoord[];
layout(location = 3) in vec3 inTangent[];
layout(location = 4) in vec3 inBitangent[];
layout(location = 5) in vec2 inTexCoord2[];
layout(location = 6) in vec4 inClipPos[];
layout(location = 7) in vec4 inWeldN[];
layout(location = 8) in vec4 inClipN[];
// x the view depth, y the weld normal's dot with the direction to the camera,
// z the unit's share of the budget (0-1).
layout(location = 9) in vec3 inView[];

layout(location = 0) out vec3 outFragPos[];
layout(location = 1) out vec3 outNormal[];
layout(location = 2) out vec2 outTexCoord[];
layout(location = 3) out vec3 outTangent[];
layout(location = 4) out vec3 outBitangent[];
layout(location = 5) out vec2 outTexCoord2[];
layout(location = 6) out vec4 outClipPos[];
layout(location = 7) out vec4 outWeldN[];
layout(location = 8) out vec4 outClipN[];

// Full level inside kNear yards, none (1) past kFar: must match
// CharacterRenderer::kFidelityTessFar.
const float kNear = 6.0;
const float kFar = 22.0;
const float kMaxLevel = 4.0;

float edgeLevel(int a, int b) {
    float depth = 0.5 * (inView[a].x + inView[b].x);
    float near = clamp((kFar - depth) / (kFar - kNear), 0.0, 1.0);
    // Only the outline shows the facets: a face turned to the camera is
    // already smooth under the per-pixel light. An edge is on the outline
    // when either corner is close to edge-on, or the two face opposite ways.
    float fa = inView[a].y, fb = inView[b].y;
    float edgeOn = fa * fb < 0.0 ? 1.0
                 : 1.0 - smoothstep(0.2, 0.55, min(abs(fa), abs(fb)));
    return mix(1.0, kMaxLevel, near * edgeOn * inView[a].z);
}

void main() {
    outFragPos[gl_InvocationID] = inFragPos[gl_InvocationID];
    outNormal[gl_InvocationID] = inNormal[gl_InvocationID];
    outTexCoord[gl_InvocationID] = inTexCoord[gl_InvocationID];
    outTangent[gl_InvocationID] = inTangent[gl_InvocationID];
    outBitangent[gl_InvocationID] = inBitangent[gl_InvocationID];
    outTexCoord2[gl_InvocationID] = inTexCoord2[gl_InvocationID];
    outClipPos[gl_InvocationID] = inClipPos[gl_InvocationID];
    outWeldN[gl_InvocationID] = inWeldN[gl_InvocationID];
    outClipN[gl_InvocationID] = inClipN[gl_InvocationID];
    if (gl_InvocationID == 0) {
        float e0 = edgeLevel(1, 2);
        float e1 = edgeLevel(2, 0);
        float e2 = edgeLevel(0, 1);
        gl_TessLevelOuter[0] = e0;
        gl_TessLevelOuter[1] = e1;
        gl_TessLevelOuter[2] = e2;
        gl_TessLevelInner[0] = max(e0, max(e1, e2));
    }
}
