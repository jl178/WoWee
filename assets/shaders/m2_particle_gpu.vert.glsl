#version 450

// M2 emitter particles from compact records. The CPU writes position, life
// ratio, its emitter's curve offset and the tile+flags word per particle;
// colour, alpha and size come from the emitter's lifetime curves here, with
// the model-class adjustments the CPU path makes. The C++ twin of this
// arithmetic is m2_particle::lookFromPacked (m2_particle_look.hpp), which the
// tests compare against the CPU path; keep the two in step.

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
    vec4 playerPos;
    vec4 playerWake;
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

// The frame's curves, kCurveVec4s each: kSteps + 1 colour+alpha entries, then
// the kSteps + 1 scales four to a vec4 (m2_particle::packCurve).
layout(std430, set = 2, binding = 0) readonly buffer ParticleCurves {
    vec4 curveData[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aLifeRatio;
layout(location = 2) in uint aCurveBase;
layout(location = 3) in uint aTileFlags;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vTile;
layout(location = 2) out float vFogVisibility;

const int kSteps = 64;
const uint kPlainDoodad   = 1u;
const uint kFlameFixture  = 2u;
const uint kSpellEffect   = 4u;
const uint kFirefly       = 8u;
const uint kAdditiveBlend = 16u;

// See m2_particle.vert.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

void main() {
    float f = clamp(aLifeRatio, 0.0, 1.0) * float(kSteps);
    int k = min(int(f), kSteps - 1);
    float t = f - float(k);
    uint uk = uint(k);
    vec4 ca = mix(curveData[aCurveBase + uk], curveData[aCurveBase + uk + 1u], t);
    uint sb = aCurveBase + uint(kSteps + 1);
    float s0 = curveData[sb + uk / 4u][int(uk & 3u)];
    float s1 = curveData[sb + (uk + 1u) / 4u][int((uk + 1u) & 3u)];
    float rawScale = mix(s0, s1, t);

    vec3 color = ca.rgb;
    float alpha = ca.a;
    uint flags = aTileFlags >> 24;
    if ((flags & kPlainDoodad) != 0u) {
        color = mix(color, vec3(1.0), 0.7);
        if (rawScale > 2.0) alpha *= 0.02;
        if ((flags & kAdditiveBlend) != 0u) alpha *= 0.05;
    }
    if ((flags & kFlameFixture) != 0u) {
        color = max(color, vec3(0.50, 0.26, 0.09));
        alpha = max(alpha, 0.30);
    }
    float scale = rawScale;
    if ((flags & kSpellEffect) != 0u) {
        scale = max(rawScale, 0.15);
    } else if ((flags & kFirefly) == 0u) {
        scale = min(rawScale, 1.5);
        if ((flags & kFlameFixture) != 0u) scale = max(scale, 0.15);
    }

    // From here on as m2_particle.vert, with the size and colour above.
    vec4 viewPos4 = view * vec4(aPos, 1.0);
    float dist = -viewPos4.z;
    gl_PointSize = clamp(scale * 500.0 / max(dist, 1.0), 1.0, 128.0);
    vColor = vec4(color, alpha);
    vTile = float(aTileFlags & 0xFFFFFFu);
    float worldDist = length(viewPos.xyz - aPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    vFogVisibility = clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0);
    if (volumetricParams.x > 0.5) vFogVisibility *= fogVolumeAt(aPos).a;
    gl_Position = projection * viewPos4;
}
