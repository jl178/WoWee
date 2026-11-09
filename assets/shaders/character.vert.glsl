#version 450

#define MAX_BONES 256u  // must match CharacterRenderer::MAX_BONES
#define HEM_SLOT (MAX_BONES - 1u)  // CharacterRenderer::kHemSlot
#define CAPE_SLOT (MAX_BONES - 2u)  // CharacterRenderer::kCapeSlot

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

layout(push_constant) uniform Push {
    mat4 model;
    // xy: the first texture stage's M2 texture-animation translation (UV
    // scroll), zw the second stage's, pushed per batch by CharacterRenderer;
    // zero for batches without one.
    vec4 uvOffset;
    // x: where each stage's coordinates come from (m2fx::UvSource), bits
    // 0-1 the first stage and 2-3 the second: 0 UV0, 1 UV1, 2 env map.
    // y: bit 0 on a robe batch while its hem sways (E7, bones[HEM_SLOT]),
    // bit 1 on a cloak batch while it bends (bones[CAPE_SLOT]).
    ivec4 stages;
} push;

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in vec4 aTangent;
layout(location = 6) in vec2 aTexCoord2;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec3 Tangent;
layout(location = 4) out vec3 Bitangent;
layout(location = 5) out vec2 TexCoord2;

#ifdef CHAR_TESS
// Smoother characters (character_tess.vert.spv, CharacterRenderer
// fidelity pipelines): the vertex is skinned exactly as above, then handed
// to character.tesc/.tese with what Phong tessellation needs. aWeld is the
// normal shared by every vertex at this position (the seams' split copies
// averaged), w how much the corner may round off (0 on a crease).
layout(location = 7) in vec4 aWeld;
layout(location = 6) out vec4 ClipPos;
layout(location = 7) out vec4 WeldN;   // world, unit length; w the weight
layout(location = 8) out vec4 ClipN;   // projection * view * (WeldN, 0)
// x the view depth, y weld N . to-camera, z this unit's share of the
// budget (push.stages.z / 1000, CharacterRenderer::kFidelityMaxUnits).
layout(location = 9) out vec3 ViewFacing;
#endif

#include "m2_combiner.glsli"

vec2 stageUv(int src, vec2 env, vec2 offset) {
    if (src == 2) return env;
    return (src == 1 ? aTexCoord2 : aTexCoord) + offset;
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-8) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 n) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return safeNormalize(cross(axis, n), vec3(1.0, 0.0, 0.0));
}

void main() {
    // Bone slots past the model's own count stay identity, so clamping keeps a
    // stray index harmless instead of reading past the buffer.
    uvec4 bi = min(aBoneIndices, uvec4(MAX_BONES - 1u));
    mat4 skinMat = bones[bi.x] * aBoneWeights.x
                 + bones[bi.y] * aBoneWeights.y
                 + bones[bi.z] * aBoneWeights.z
                 + bones[bi.w] * aBoneWeights.w;

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    // Robes and skirts (E7, CharacterRenderer::stepRobeHem): the hem's lag
    // behind each leg, spread by how far below the robe's top the vertex
    // hangs and how far it stands off that side's leg; zero on the cloth
    // that hugs the shin, all of it at a free-swinging hem.
    // Cloaks (E7, CharacterRenderer::updateCapeBend): the cloak turned as a
    // plate about a line across the shoulders; nothing above that line.
    if ((push.stages.y & 2) != 0) {
        mat4 cape = bones[CAPE_SLOT];
        float below = max(cape[2].x - aPos.z, 0.0);
        float ease = smoothstep(0.0, 1.0, below / max(cape[2].x - cape[2].y, 1e-3));
        skinnedPos.xyz += (cape[0].xyz + cape[1].xyz) * (below * ease);
    }
    if ((push.stages.y & 1) != 0) {
        mat4 hem = bones[HEM_SLOT];
        float depth = clamp((hem[2].x - aPos.z) / max(hem[2].x - hem[2].y, 1e-3), 0.0, 1.0);
        float side = smoothstep(hem[3].y, hem[2].w, aPos.y);
        vec2 leg = mix(hem[3].xy, hem[2].zw, side);
        float off = smoothstep(hem[3].z, hem[3].w, length(aPos.xy - leg));
        skinnedPos.xyz += mix(hem[1].xyz, hem[0].xyz, side) * (depth * depth * off);
    }
    vec3 skinnedNorm = mat3(skinMat) * aNormal;
    vec3 skinnedTan = mat3(skinMat) * aTangent.xyz;

    vec4 worldPos = push.model * skinnedPos;
    mat3 modelMat3 = mat3(push.model);
    FragPos = worldPos.xyz;
    Normal = modelMat3 * skinnedNorm;
    // The stock vertex shaders' stage coordinates (Diffuse_T1, _T2, _Env and
    // their pairs): a UV set through the stage's transform, or the sphere map.
    int src0 = push.stages.x & 3;
    int src1 = (push.stages.x >> 2) & 3;
    vec2 env = vec2(0.0);
    if (src0 == 2 || src1 == 2) {
        env = m2SphereEnvUv((view * worldPos).xyz, mat3(view) * Normal);
    }
    TexCoord = stageUv(src0, env, push.uvOffset.xy);
    TexCoord2 = stageUv(src1, env, push.uvOffset.zw);

    // Gram-Schmidt re-orthogonalize tangent w.r.t. normal
    vec3 N = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    vec3 T = safeNormalize(modelMat3 * skinnedTan, fallbackTangent(N));
    T = safeNormalize(T - dot(T, N) * N, fallbackTangent(N));
    vec3 B = safeNormalize(cross(N, T) * aTangent.w, safeNormalize(cross(N, fallbackTangent(N)), vec3(0.0, 1.0, 0.0)));

    Tangent = T;
    Bitangent = B;

    gl_Position = projection * view * worldPos;
#ifdef CHAR_TESS
    vec3 weldWorld = safeNormalize(modelMat3 * (mat3(skinMat) * aWeld.xyz), N);
    WeldN = vec4(weldWorld, aWeld.w);
    ClipPos = gl_Position;
    ClipN = projection * view * vec4(weldWorld, 0.0);
    ViewFacing = vec3(gl_Position.w,
                      dot(weldWorld, safeNormalize(viewPos.xyz - worldPos.xyz, weldWorld)),
                      float(push.stages.z) * 0.001);
#endif
}
