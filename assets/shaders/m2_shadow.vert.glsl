#version 450

// M2 shadow casters, instanced. The model matrix comes from a per-frame
// buffer of casters sorted by model, so one draw covers every caster of a
// model where shadow.vert took a push constant and a draw per instance.
// Same push layout as shadow.vert: the fragment shader reads flags from it.
layout(push_constant) uniform Push {
    mat4 lightSpace;        // light-space only; the model is per instance
    vec4 sway;              // z reference height, w amplitude; xy unused here
    ivec4 flags;            // x useTexture, y alphaTest, z sway (1 tree wind, 2 canvas), w first caster index
    vec4 wind;              // x windTime, y foliage wind strength, z cloth wind strength
} push;

layout(std430, set = 1, binding = 0) readonly buffer Casters {
    mat4 model[];
} casters;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aBoneWeights;
layout(location = 3) in vec4 aBoneIndicesF;
layout(location = 4) in vec3 aNormal;
layout(location = 5) in float aCanvasWeight;

layout(location = 0) out vec2 TexCoord;

void main() {
    mat4 model = casters.model[push.flags.w + gl_InstanceIndex];
    vec4 pos = vec4(aPos, 1.0);
    // The tree wind of m2.vert.glsl's mode 1, line for line: the shadow of a
    // canopy has to move with the canopy (see m2_sway.hpp).
    if (push.flags.z == 1) {
        vec2 worldRef = model[3].xy;
        float windTime = push.wind.x;
        float heightFactor = clamp(pos.z / max(push.sway.z, 0.01), 0.0, 1.0);
        heightFactor *= heightFactor;   // quadratic - the base stays planted
        float amp = push.sway.w * heightFactor * push.wind.y;
        const vec2 kWindDir = normalize(vec2(0.80, 0.60));
        float gust = 0.55 + 0.45 * sin(dot(worldRef, kWindDir) * 0.11 - windTime * 0.7);
        vec2 windM = (transpose(mat3(model)) * vec3(kWindDir, 0.0)).xy;
        windM *= inversesqrt(max(dot(windM, windM), 1e-8));
        vec2 crossM = vec2(-windM.y, windM.x);
        float trunkPhase = windTime * 0.8 + dot(worldRef, vec2(0.1, 0.13));
        float trunkAlong = (0.12 + 0.23 * sin(trunkPhase)) * gust * amp;
        float trunkAcross = cos(trunkPhase * 0.7) * 0.08 * amp;
        float trunkSwayX = windM.x * trunkAlong + crossM.x * trunkAcross;
        float trunkSwayY = windM.y * trunkAlong + crossM.y * trunkAcross;
        float branchPhase = push.wind.x * 1.7 + dot(worldRef, vec2(0.37, 0.71));
        float branchSwayX = sin(branchPhase + pos.y * 0.4) * 0.15 * amp;
        float branchSwayY = cos(branchPhase * 1.1 + pos.x * 0.3) * 0.12 * amp;
        float leafPhase = push.wind.x * 4.5 + dot(aPos, vec3(1.7, 2.3, 0.9));
        float leafFlutterX = sin(leafPhase) * 0.06 * amp;
        float leafFlutterY = cos(leafPhase * 1.3) * 0.05 * amp;
        pos.x += trunkSwayX + branchSwayX + leafFlutterX;
        pos.y += trunkSwayY + branchSwayY + leafFlutterY;
    }
    // Canvas on a frame, m2.vert.glsl's mode 5.
    if (push.flags.z == 2) {
        float ph = push.wind.x * 1.3 + dot(model[3].xy, vec2(0.21, 0.17));
        float flutter = 0.65 * sin(ph) + 0.35 * sin(2.3 * ph + dot(aPos.xy, vec2(1.3, 1.7)));
        vec3 canvasN = aNormal * inversesqrt(max(dot(aNormal, aNormal), 1e-8));
        pos.xyz += canvasN * (aCanvasWeight * push.sway.w * flutter * push.wind.z);
    }
    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * model * pos;
}
