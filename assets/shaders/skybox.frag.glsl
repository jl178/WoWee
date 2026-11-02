#version 450

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

layout(push_constant) uniform Push {
    vec4 topColor;        // LightIntBand 2, the zenith
    vec4 middleColor;     // 3, ring 1 (about 16.8 degrees up)
    vec4 band1Color;      // 4, ring 2 (9.8)
    vec4 band2Color;      // 5, ring 3 (3.7)
    vec4 smogColor;       // 6, ring 4 (1.8)
    vec4 fogColorPush;    // 7, the horizon and everything below it
    vec4 params;          // x = dawn/dusk highlight (0x7f0530), yzw unused
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

// The stock dome (0x7f2470): a unit sphere lowered cos(pi/4) below the eye,
// rings at these polar angles (fractions of pi), Gouraud-shaded - so the
// colour runs linearly in polar angle from ring to ring.
const float kRingTheta[7] = float[](0.0, 0.17, 0.20, 0.23, 0.24, 0.25, 1.0);

// 0x7ed3b0 over a six-key table: linear between keys, wrapping round.
float sampleKeys6(const vec2 k[6], float t) {
    int hi = 0;
    while (hi < 6 && t > k[hi].x) ++hi;
    int lo;
    if (hi == 6 || hi == 0) { hi = 0; lo = 5; } else { lo = hi - 1; }
    float span = k[hi].x - k[lo].x;
    if (abs(span) < 0.001) return k[lo].y;
    if (span < 0.0) span += 1.0;
    float into = t - k[lo].x;
    if (into < 0.0) into += 1.0;
    return mix(k[lo].y, k[hi].y, into / span);
}

// 0xaf4bac: the highlight round the horizon by world azimuth.
const vec2 kAzimuthGlow[6] = vec2[](vec2(0.125, 1.0), vec2(0.375, 0.0), vec2(0.5, -0.5),
                                    vec2(0.625, -0.7), vec2(0.75, -0.5), vec2(0.875, 0.0));

// One ring's colour as 0x7f0530 writes its vertices.
vec3 ringColor(int ring, float g, float h) {
    if (ring <= 0) return push.topColor.rgb;
    if (ring >= 5) return push.fogColorPush.rgb;
    vec3 normal = ring == 1 ? push.middleColor.rgb
                : ring == 2 ? push.band1Color.rgb
                : ring == 3 ? push.band2Color.rgb : push.smogColor.rgb;
    if (h <= 0.0) return normal;
    vec3 lit = mix(normal, push.middleColor.rgb, h);
    if (g >= 0.0) return mix(normal, lit, (1.0 - g) * h);
    return mix(lit, mix(lit, push.topColor.rgb, h * 0.7), -g * h);
}

void main() {
    // Reconstruct world-space ray direction from screen position.
    float ndcX =  TexCoord.x * 2.0 - 1.0;
    float ndcY = -(TexCoord.y * 2.0 - 1.0);

    vec3 viewDir = vec3(ndcX / projection[0][0],
                        ndcY / abs(projection[1][1]),
                        -1.0);

    mat3 invViewRot = transpose(mat3(view));
    vec3 worldDir = normalize(invViewRot * viewDir);

    // Where the eye ray meets the lowered sphere, as a polar angle.
    const float h0 = 0.70710678;
    float dz = clamp(worldDir.z, -1.0, 1.0);
    float t = -h0 * dz + sqrt(max(h0 * h0 * dz * dz - h0 * h0 + 1.0, 0.0));
    float polar = acos(clamp(t * dz + h0, -1.0, 1.0)) / 3.14159265;
    int ring = 5;
    float f = 1.0;
    for (int i = 0; i < 6; ++i) {
        if (polar <= kRingTheta[i + 1]) {
            ring = i;
            f = (polar - kRingTheta[i]) / (kRingTheta[i + 1] - kRingTheta[i]);
            break;
        }
    }
    float h = push.params.x;
    // World azimuth: WoW's atan2(y, x), in render components (x = wowY).
    float g = 0.0;
    if (h > 0.0) {
        float az = atan(worldDir.x, worldDir.y) / 6.28318531;
        g = sampleKeys6(kAzimuthGlow, az - floor(az));
    }
    vec3 sky = mix(ringColor(ring, g, h), ringColor(ring + 1, g, h), clamp(f, 0.0, 1.0));

    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeSky(TexCoord);
        sky = sky * air.a + air.rgb;
    }

    outColor = vec4(sky, 1.0);
}
