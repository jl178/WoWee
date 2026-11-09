#version 450

// Enhanced haze - the "Sun haze (the reference client)" setting (not stock;
// docs/re/enhanced-lighting.md section 5). Drawn by ContactShadowPass with
// this shader, blended src.rgb + dst * src.a, once the opaque scene is done.
//
// Enhanced's global fog (volumefogglobalfog.bls) puts layers of air into its
// froxel volume, each with a start distance, a density, an exponential fade
// with height and in-scattering of the sun through a forward phase lookup,
// out to its 7000-yard far clip. Wowee's own froxel mist covers the first 400
// yards; this is the same layer model over the whole view, integrated in
// closed form along each pixel's ray instead of in cells:
//
//   density(z)  = d0 * exp(-(z - h0) / H)        (lower air is thicker)
//   depth(L)    = d0 * exp(-(c.z - h0) / H) * H * (1 - exp(-v.z L / H)) / v.z
//   T           = exp(-depth(max(L - start, 0)))
//   added light = (sky * kSkyScatter + sun * phase(cos) * kSunScatter) * (1 - T)
//
// with h0 the ground under the player, c the camera, v the ray. The phase is
// Henyey-Greenstein, g 0.7, mixed 60/40 with even scattering - Enhanced's
// lookup table itself was not read. The sky takes the ray to kSkyDistance, so
// the air around a low sun glows gold and the horizon pales; the sun's
// shadow is left to the froxel mist near the camera, which is where shafts
// show.

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
};

layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

layout(push_constant) uniform Push {
    mat4 invViewProj;  // clip -> world, this frame's
    vec4 target;       // xy the render extent, z strength (0..2; weather thins it), w flags (4 debug)
} pc;

layout(location = 0) out vec4 outColor;

// Thin: Jered's first evening in Goldshire with 0.0006 from 40 yards was
// a grey wall over the whole view. Enhanced's near field is clear; its air
// shows over hundreds of yards. Now the density also ramps in over kRamp
// past the start, so nothing near the player is hazed at all.
const float kDensity = 0.00022;      // per yard at the ground under the player
const float kHeightScale = 220.0;    // yards for the air to thin by e
const float kStart = 120.0;          // yards of clear air in front of the camera
const float kRamp = 400.0;           // yards over which the haze reaches full density
const float kSkyDistance = 5000.0;   // how far a sky pixel's ray runs
// The haze's colour is the sun's, scattered forward: gold toward a low sun,
// little elsewhere. The sky's own share is small and taken from the fog
// colour, which is grey under cloud - most of what made the wall grey.
const float kSkyScatter = 0.35;      // of the fog colour, light from the sky
const float kSunScatter = 0.8;       // of the sun colour, at even scattering
const float kG = 0.7;

float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5);
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    float d = texelFetch(uSceneDepth, px, 0).r;
    vec2 uv = gl_FragCoord.xy / pc.target.xy;
    vec4 w = pc.invViewProj * vec4(uv * 2.0 - 1.0, min(d, 0.9999), 1.0);
    vec3 p = w.xyz / w.w;
    vec3 cam = viewPos.xyz;
    vec3 v = p - cam;
    float len = length(v);
    if (len < 1e-3) discard;
    v /= len;
    float L = d >= 1.0 ? kSkyDistance : len;
    float run = max(L - kStart, 0.0);
    if (run <= 0.0) discard;
    // The ramp: the first kRamp yards past the start count at a growing
    // share, as if the start were soft (half of them, in all).
    run = run > kRamp ? run - 0.5 * kRamp : 0.5 * run * run / kRamp;

    float h0 = playerPos.z;
    float base = kDensity * exp(-(cam.z + v.z * kStart - h0) / kHeightScale);
    float k = v.z / kHeightScale;
    float depthOD = abs(k) < 1e-5 ? base * run : base * (1.0 - exp(-k * run)) / k;
    float strength = clamp(pc.target.z, 0.0, 2.0);
    float T = exp(-depthOD * strength);

    vec3 travel = normalize(lightDir.xyz);
    float sunUp = smoothstep(0.0, 0.08, -travel.z);
    float phase = mix(1.0, phaseHG(dot(-travel, v), kG), 0.6);
    vec3 light = fogColor.rgb * kSkyScatter + lightColor.rgb * (phase * kSunScatter * sunUp);
    // Capped near white: past it the haze only blows a bright sky and the
    // land under it out (the Barrens toward an evening sun, round 4).
    light = min(light, vec3(1.3));
    // Indoors the outdoor air stays outside.
    float outdoors = 1.0 - clamp(shadowParams.w, 0.0, 1.0);
    float a = mix(1.0, T, outdoors);
    if (a >= 0.999) discard;
    if ((int(pc.target.w + 0.5) & 4) != 0) {
        outColor = vec4(vec3(1.0 - a), 0.0);   // WOWEE_CONTACT_SHADOW_DEBUG: how much air
        return;
    }
    outColor = vec4(light * (1.0 - a), a);
}
