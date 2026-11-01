#version 450

// M2 shadow casters, instanced. The model matrix comes from a per-frame
// buffer of casters sorted by model, so one draw covers every caster of a
// model where shadow.vert took a push constant and a draw per instance.
// Same push layout as shadow.vert: the fragment shader reads flags from it.
layout(push_constant) uniform Push {
    mat4 lightSpace;        // light-space only; the model is per instance
    vec4 sway;              // z reference height, w amplitude; xy unused here
    ivec4 flags;            // x useTexture, y alphaTest, z foliageSway, w first caster index
    vec4 wind;              // x windTime
} push;

layout(std430, set = 1, binding = 0) readonly buffer Casters {
    mat4 model[];
} casters;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aBoneWeights;
layout(location = 3) in vec4 aBoneIndicesF;

layout(location = 0) out vec2 TexCoord;

void main() {
    mat4 model = casters.model[push.flags.w + gl_InstanceIndex];
    vec4 pos = vec4(aPos, 1.0);
    if (push.flags.z != 0) {
        vec2 worldRef = model[3].xy;
        float heightFactor = clamp(pos.z / max(push.sway.z, 0.01), 0.0, 1.0);
        heightFactor *= heightFactor;   // quadratic - the base stays planted
        float amp = push.sway.w * heightFactor;
        float trunkPhase = push.wind.x * 0.8 + dot(worldRef, vec2(0.1, 0.13));
        float trunkSwayX = sin(trunkPhase) * 0.35 * amp;
        float trunkSwayY = cos(trunkPhase * 0.7) * 0.25 * amp;
        float branchPhase = push.wind.x * 1.7 + dot(worldRef, vec2(0.37, 0.71));
        float branchSwayX = sin(branchPhase + pos.y * 0.4) * 0.15 * amp;
        float branchSwayY = cos(branchPhase * 1.1 + pos.x * 0.3) * 0.12 * amp;
        float leafPhase = push.wind.x * 4.5 + dot(aPos, vec3(1.7, 2.3, 0.9));
        float leafFlutterX = sin(leafPhase) * 0.06 * amp;
        float leafFlutterY = cos(leafPhase * 1.3) * 0.05 * amp;
        pos.x += trunkSwayX + branchSwayX + leafFlutterX;
        pos.y += trunkSwayY + branchSwayY + leafFlutterY;
    }
    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * model * pos;
}
