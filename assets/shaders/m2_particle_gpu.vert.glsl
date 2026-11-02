#version 450

// M2 emitter particles as the stock client draws them: one quad per record,
// expanded here from four strip vertices per instance (CParticleEmitter2's
// quad builder, Wow.exe 0x97BE80; docs/re/particles.md). The CPU writes the
// particle's position, life ratio and the per-particle factors; the colour,
// alpha and size come from the emitter's lifetime curves in the curve buffer,
// evaluated as the stock evaluator does (0x9793B0). The C++ twin of the curve
// read is m2_particle::lookFromPacked, of the corners m2_particle::*Corner;
// keep them in step.

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

// Each emitter's curves, kCurveVec4s (25) vec4s: a header with the key
// counts, then 8 colour keys (rgb, time), 8 alpha keys (a, time) and 8 scale
// keys (x, y, time) - m2_particle::packCurve.
layout(std430, set = 2, binding = 0) readonly buffer ParticleCurves {
    vec4 curveData[];
};

layout(push_constant) uniform Push {
    vec2 tileCount;  // columns, rows
    uint blendType;
    uint pad;
} push;

// One record per instance (m2_particle::ParticleRecord).
layout(location = 0) in vec4 aPosLife;   // world position, life ratio
layout(location = 1) in vec4 aVecSize;   // per kind (see below), size factor x
layout(location = 2) in vec2 aAngleSize; // spin angle, size factor y
layout(location = 3) in uvec2 aCurvePacked;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) out float vFogVisibility;

const uint kKeys = 8u;
const uint kKindBillboard = 0u;
const uint kKindVelocity = 1u;
const uint kKindXYQuad = 2u;
const uint kKindTail = 3u;

// The stock corner table (0xB2D5B4) and its texture coordinates (0xB2D5D4):
// a strip, top left first. A corner is a whole size from the centre.
const vec2 kCorner[4] = vec2[](vec2(-1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
const vec2 kCornerUV[4] = vec2[](vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));

// See m2_particle.vert.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// m2_particle::fblockSegment for a track whose time is component tc of each
// key: the key pair a life ratio falls in and the fraction between them.
void segment(uint keys, uint n, int tc, float r, out uint i0, out uint i1, out float f) {
    i0 = 0u; i1 = 0u; f = 0.0;
    if (n == 2u) { i1 = 1u; f = r; return; }
    if (n == 3u) {
        float mid = curveData[keys + 1u][tc];
        if (r < mid) { i1 = 1u; f = r / mid; }
        else { i0 = 1u; i1 = 2u; f = (r - mid) / (1.0 - mid); }
        return;
    }
    for (uint k = 1u; k + 1u < n; ++k)
        if (curveData[keys + k][tc] <= r) i0 = k;
    i1 = i0 + 1u;
    float t0 = curveData[keys + i0][tc];
    float d = curveData[keys + i1][tc] - t0;
    f = d != 0.0 ? (r - t0) / d : 0.0;
}

vec2 rotate(vec2 v, float a) {
    float c = cos(a), s = sin(a);
    return vec2(c * v.x - s * v.y, s * v.x + c * v.y);
}

void main() {
    int corner = gl_VertexIndex & 3;
    uint base = aCurvePacked.x;
    uint packed = aCurvePacked.y;
    float r = aPosLife.w;

    // Curves (0x979E90): colour, alpha, scale over the particle's life.
    vec4 h = curveData[base];
    uint nc = uint(h.x), na = uint(h.y), ns = uint(h.z);
    uint ck = base + 1u, ak = ck + kKeys, sk = ak + kKeys;
    uint i0, i1; float f;
    vec3 color = vec3(1.0);
    if (nc == 1u) color = curveData[ck].rgb;
    else if (nc > 1u) { segment(ck, nc, 3, r, i0, i1, f); color = mix(curveData[ck + i0].rgb, curveData[ck + i1].rgb, f); }
    float alpha = 1.0;
    if (na == 1u) alpha = curveData[ak].x;
    else if (na > 1u) { segment(ak, na, 1, r, i0, i1, f); alpha = mix(curveData[ak + i0].x, curveData[ak + i1].x, f); }
    vec2 scale = vec2(1.0);
    if (ns == 1u) scale = curveData[sk].xy;
    else if (ns > 1u) { segment(sk, ns, 2, r, i0, i1, f); scale = mix(curveData[sk + i0].xy, curveData[sk + i1].xy, f); }
    vec2 size = scale * vec2(aVecSize.w, aAngleSize.y);

    uint kind = (packed >> 16) & 3u;
    bool spin = (packed & (1u << 18)) != 0u;
    float angle = aAngleSize.x;
    vec3 right = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 up = vec3(view[0][1], view[1][1], view[2][1]);
    vec3 pos = aPosLife.xyz;
    vec3 world;
    vec2 cuv = kCornerUV[corner];
    if (kind == kKindTail) {
        // A quad from the head back along the tail, `size` either side of it
        // across the screen (0x97CC80). Too short on screen: a plain quad.
        vec3 t = aVecSize.xyz;
        vec2 tv = vec2(dot(t, right), dot(t, up));
        float len2 = dot(tv, tv);
        if (len2 > 0.0007716049440205097) {
            vec2 p = vec2(-size.y * tv.y, size.x * tv.x) * inversesqrt(len2);
            vec3 pw = right * p.x + up * p.y;
            world = pos + (corner >= 2 ? t : vec3(0.0)) + ((corner & 1) == 0 ? pw : -pw);
        } else {
            vec2 o = kCorner[corner] * size;
            world = pos + right * o.x + up * o.y;
        }
    } else if (kind == kKindVelocity) {
        // Stretched along the velocity's projection on the screen, shortened
        // by how much it points at the camera (0x97C82x).
        vec3 v = aVecSize.xyz;
        vec2 vv = vec2(dot(v, right), dot(v, up));
        float len2 = dot(vv, vv);
        float inv2 = len2 <= 2.384185791015625e-07 ? 0.0 : inversesqrt(len2);
        float sx = size.x;
        if (inv2 > 2.384185791015625e-07) sx = size.x * inversesqrt(dot(v, v)) / inv2;
        vec2 d = vv * inv2;
        float a = kCorner[corner].x * sx, b = kCorner[corner].y * size.y;
        vec2 o = vec2(a * d.x - b * d.y, b * d.x + a * d.y);
        world = pos + right * o.x + up * o.y;
    } else {
        vec2 o = kCorner[corner] * size;
        if (spin) o = rotate(o, angle);
        if (kind == kKindXYQuad) {
            // In the particle space's XY plane, turned by the quaternion.
            vec3 q = aVecSize.xyz;
            float w = sqrt(max(1.0 - dot(q, q), 0.0));
            vec3 ax = vec3(1.0, 0.0, 0.0) + 2.0 * cross(q, cross(q, vec3(1.0, 0.0, 0.0)) + w * vec3(1.0, 0.0, 0.0));
            vec3 ay = vec3(0.0, 1.0, 0.0) + 2.0 * cross(q, cross(q, vec3(0.0, 1.0, 0.0)) + w * vec3(0.0, 1.0, 0.0));
            world = pos + ax * o.x + ay * o.y;
        } else {
            world = pos + right * o.x + up * o.y;
        }
    }

    // The cell: columns across, rows down (0x978C70).
    uint cell = packed & 0xFFFFu;
    vec2 tiles = max(push.tileCount, vec2(1.0));
    uint cols = uint(tiles.x);
    vec2 cellOrigin = vec2(float(cell % cols), float(cell / cols));
    vUV = (cellOrigin + cuv) / tiles;

    float modelAlpha = float(packed >> 24) / 255.0;
    alpha = clamp(alpha, 0.0, 1.0) * modelAlpha;
    if ((packed & (1u << 19)) != 0u) {
        // Lit: the stock gives the quad the camera's back vector as its normal
        // (0xB2D540) and lights it with the scene's ambient and sun.
        vec3 n = vec3(view[0][2], view[1][2], view[2][2]);
        float diff = max(dot(n, normalize(-lightDir.xyz)), 0.0);
        color *= min(ambientColor.rgb + lightColor.rgb * diff, vec3(1.0));
    }
    vColor = vec4(color, alpha);

    vec4 viewPos4 = view * vec4(world, 1.0);
    if ((packed & (1u << 20)) != 0u) {
        vFogVisibility = 1.0;
    } else {
        float worldDist = length(viewPos.xyz - world);
        float fogRange = max(fogParams.y - fogParams.x, 0.001);
        vFogVisibility = clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0);
        if (volumetricParams.x > 0.5) vFogVisibility *= fogVolumeAt(world).a;
    }
    gl_Position = projection * viewPos4;
}
