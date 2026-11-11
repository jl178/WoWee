#version 450

// Walks one column of the fog volume away from the camera. See
// volumetric_fog.hpp.
//
// At every slice it stores what the air from the camera to that slice's far
// edge adds up to: the light it scatters toward the camera (rgb) and how much
// of whatever lies behind still shows through it (a). A surface then reads
// the slice it stands in and takes color * a + rgb.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

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

layout(set = 1, binding = 0) uniform VolumeParams {
    vec4 rayOrigin;
    vec4 rayCorner[4];
    mat4 prevViewProj;
    vec4 medium;
    vec4 lighting;
    vec4 drift;
    vec4 jitter;
    ivec4 dims;
    vec4 shafts;
} vol;

layout(set = 1, binding = 4) uniform sampler3D uCells;
layout(set = 1, binding = 5, rgba16f) uniform writeonly image3D uIntegratedOut;

float sliceDepth(float s) {
    return volumetricParams.y * exp(s / (volumetricParams.w * volumetricParams.z));
}

vec3 rayAt(vec2 uv) {
    vec3 top = mix(vol.rayCorner[0].xyz, vol.rayCorner[1].xyz, uv.x);
    vec3 bottom = mix(vol.rayCorner[2].xyz, vol.rayCorner[3].xyz, uv.x);
    return mix(top, bottom, uv.y);
}

// Under Enhanced's light (localLightMeta.z bit 128) the light the air adds in
// front of a surface is held under a ceiling set by the sky behind it - the
// zone's fog colour, which the sky is painted to meet at the horizon - plus
// a little of the sun's own colour, so a dark night sky does not choke the
// moonbeams. Up to half the ceiling it is untouched; past that it eases
// toward the ceiling instead of climbing without end. Toward a sun just over
// the horizon the view looks along hundreds of yards of lit air, and the sum
// took the ground and the sky round the sun to white: Westfall at 6:18 had
// 7.6% of the view white, 4.3% with the lobe's pull-back restored alone
// (docs/re/enhanced-lighting.md 5c). Shafts in a forest, moonbeams and the
// midday air sit under the knee and keep their light.
const float kMistCeiling = 1.0;
const float kMistCeilingSun = 0.35;

vec3 softCeiling(vec3 light, vec3 ceiling) {
    vec3 knee = 0.5 * ceiling;
    vec3 over = max(light - knee, vec3(0.0));
    vec3 room = ceiling - knee;
    return min(light, knee) + room * (1.0 - exp(-over / room));
}

void main() {
    ivec2 column = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(column, vol.dims.xy))) return;

    // The slices are depths along the view; off the centre of the screen
    // the ray crosses each one at a slant, so it passes through more air.
    float slant = length(rayAt((vec2(column) + 0.5) / vec2(vol.dims.xy)));

    vec3 scattered = vec3(0.0);
    bool capped = (localLightMeta.z & 128) != 0;
    vec3 ceiling = max(kMistCeiling * fogColor.rgb + kMistCeilingSun * lightColor.rgb, vec3(0.02));
    float transmittance = 1.0;
    float front = sliceDepth(0.0);
    for (int z = 0; z < vol.dims.z; ++z) {
        float back = sliceDepth(float(z + 1));
        float span = (back - front) * slant;
        front = back;

        vec4 cell = texelFetch(uCells, ivec3(column, z), 0);
        float extinction = max(cell.a, 1e-6);
        float through = exp(-extinction * span);
        // The cell's light integrated across its own depth, each yard of it
        // dimmed by the air in front of it within the cell, rather than all
        // of it taken at the front - which over-brightens a thick cell and
        // makes the result depend on how finely the depth is sliced.
        scattered += transmittance * (cell.rgb - cell.rgb * through) / extinction;
        transmittance *= through;

        imageStore(uIntegratedOut, ivec3(column, z),
                   vec4(capped ? softCeiling(scattered, ceiling) : scattered, transmittance));
    }
}
