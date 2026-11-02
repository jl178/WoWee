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
    vec4 cloudColor;      // xyz = cloud sun colour (LightIntBand 10), w = sun factor (1 - 0.75 storm)
    vec4 sunDirDensity;   // xyz = the lighting body's direction (sun by day, moon by night), w = cover (LightFloatBand 3)
    vec4 windAndLight;    // x = noise time (cells), y = light height in texels (64 + 192 storm), zw unused
    vec4 cloudShade;      // xyz = cloud emissive (LightIntBand 11)
    vec4 layerAmbient;    // xyz = cloud layer 1 ambient (LightIntBand 12)
} push;

layout(location = 0) in vec3 vWorldDir;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

// The stock cloud layer (0x7efd00 builds it into a 128x128 texture on the
// CPU, 0x7f20e0 the dome it is drawn on). Five octaves of 3D value noise -
// the third axis is time, so the clouds churn in place rather than drift -
// at 1/16 .. 1 cell a texel and weights 1 .. 1/16; a random table stands in
// for the client's (0xd38688, filled at random on start).
float hash3(vec3 c) {
    return fract(sin(dot(c, vec3(127.1, 311.7, 74.7))) * 43758.5453) * 2.0 - 1.0;
}
float valueNoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float a = mix(hash3(i), hash3(i + vec3(1, 0, 0)), u.x);
    float b = mix(hash3(i + vec3(0, 1, 0)), hash3(i + vec3(1, 1, 0)), u.x);
    float c = mix(hash3(i + vec3(0, 0, 1)), hash3(i + vec3(1, 0, 1)), u.x);
    float d = mix(hash3(i + vec3(0, 1, 1)), hash3(i + vec3(1, 1, 1)), u.x);
    return mix(mix(a, b, u.y), mix(c, d, u.y), u.z);
}
// Returns x = the full sum, y = the first three octaves (what the client
// takes its slopes from).
vec2 cloudNoise(vec2 texel, float t) {
    float sum = 0.0, low = 0.0;
    float cellsPerTexel = 1.0 / 16.0;
    float w = 1.0;
    for (int k = 0; k < 5; ++k) {
        sum += valueNoise(vec3(texel * cellsPerTexel, t + float(k) * 17.0)) * w;
        if (k == 2) low = sum;
        cellsPerTexel *= 2.0;
        w *= 0.5;
    }
    return vec2(sum, low);
}

float lowNoise(vec2 texel, float t) {
    float sum = 0.0;
    float cellsPerTexel = 1.0 / 16.0;
    float w = 1.0;
    for (int k = 0; k < 3; ++k) {
        sum += valueNoise(vec3(texel * cellsPerTexel, t + float(k) * 17.0)) * w;
        cellsPerTexel *= 2.0;
        w *= 0.5;
    }
    return sum;
}

const float kRing[12] = float[](0.0, 0.025, 0.05, 0.075, 0.1, 0.125, 0.15, 0.175,
                                0.205, 0.23, 0.245, 0.25);
const float kRingAlpha[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
                                     0.50196, 0.0, 0.0);

void main() {
    vec3 dir = normalize(vWorldDir);

    // Where the ray meets the cloud dome (the sky's sphere, lowered
    // cos(pi/4)), as a polar fraction; the dome stops at the horizon.
    const float h0 = 0.70710678;
    float dz = clamp(dir.z, -1.0, 1.0);
    float tHit = -h0 * dz + sqrt(max(h0 * h0 * dz * dz - h0 * h0 + 1.0, 0.0));
    float polar = acos(clamp(tHit * dz + h0, -1.0, 1.0)) / 3.14159265;
    if (polar >= 0.25) discard;
    float ring = 11.0;
    for (int i = 0; i < 11; ++i) {
        if (polar <= kRing[i + 1]) {
            ring = float(i) + (polar - kRing[i]) / (kRing[i + 1] - kRing[i]);
            break;
        }
    }
    int ri = min(int(ring), 10);
    float vertexAlpha = mix(kRingAlpha[ri], kRingAlpha[ri + 1], ring - float(ri));
    if (vertexAlpha <= 0.0) discard;

    // Polar texture coordinates: radius ring/22 round the texture's centre.
    vec2 horiz = length(dir.xy) > 1e-5 ? normalize(dir.xy) : vec2(0.0, 1.0);
    vec2 texel = (vec2(0.5) + horiz * (ring / 22.0)) * 128.0;

    float t = push.windAndLight.x;
    vec2 n = cloudNoise(texel, t);
    float cover = clamp(push.sunDirDensity.w, 0.0, 1.0);
    float v = floor(n.x * 64.0 + 128.5) - floor((1.0 - cover) * 255.0 + 0.5);
    if (v <= 0.0) discard;
    float alpha = 1.0 - pow(0.96, min(v, 255.0));

    // Slope of the low octaves, a texel each way, against a light hung over
    // the body's place on the texture.
    float gx = lowNoise(texel - vec2(1.0, 0.0), t) - n.y;
    float gy = lowNoise(texel - vec2(0.0, 1.0), t) - n.y;
    vec3 N = normalize(vec3(gx, gy, 1.0));
    vec3 body = normalize(push.sunDirDensity.xyz);
    float fromZenith = min(acos(clamp(body.z, -1.0, 1.0)), 0.78539816) * 1.2732395 * 0.5;
    vec2 bodyHoriz = length(body.xy) > 1e-5 ? normalize(body.xy) : vec2(0.0);
    vec2 bodyTexel = (vec2(0.5) + bodyHoriz * fromZenith) * 128.0;
    vec3 L = normalize(vec3(bodyTexel - texel, push.windAndLight.y));
    float nDotL = dot(N, L);

    float a255 = floor(alpha * 255.0 + 0.5);
    float shade = (floor((255.0 - a255) * 0.5) + 64.0) / 255.0;
    vec3 cloudRgb = push.cloudShade.rgb * shade + push.layerAmbient.rgb;
    if (nDotL > 0.0) cloudRgb += push.cloudColor.rgb * (nDotL * push.cloudColor.w);
    cloudRgb = min(cloudRgb, vec3(1.0));

    alpha *= vertexAlpha;
    if (alpha < 0.004) discard;

    // Behind the air, as the sky it is drawn over is: blended by alpha, so
    // the cloud and the dome under it come out with the same air in front.
    if (volumetricParams.x > 0.5) {
        vec4 clip = projection * mat4(mat3(view)) * vec4(vWorldDir, 1.0);
        vec4 air = fogVolumeSky(clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5);
        cloudRgb = cloudRgb * air.a + air.rgb;
    }
    outColor = vec4(cloudRgb, alpha);
}
