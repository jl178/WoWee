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
    vec4 target;       // xy the render extent, z strength (0..2; weather thins it), w flags (4 debug, 8 HDR scene)
    vec4 air;          // Enhanced's air: a its density per yard at the ground (0 = none);
                       // r the forward lobe's share (0 under a sky with no sun disc); gb unused
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
// 0.65 (was 0.8): with the world's light now from Enhanced's sun (the
// Enhanced daylight), the forward lobe sits on the sun's disc rather than
// beside it, and at 0.8 the sky round the sun had 15% more near-white.
const float kSunScatter = 0.65;      // of the sun colour, at even scattering
const float kG = 0.7;
const float kMaxLight = 0.88;        // the 8-bit scene's brightest haze channel
const float kMaxLightHdr = 1.5;      // and the HDR scene's (enhanced_tonemap::kHazeCap)

float phaseHG(float cosTheta, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
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
    float h0 = playerPos.z;
    float strength = clamp(pc.target.z, 0.0, 2.0);
    // Indoors the outdoor air stays outside.
    float outdoors = 1.0 - clamp(shadowParams.w, 0.0, 1.0);

    // Enhanced's air: its global fog layer (volumefogglobalfog.bls; the
    // layer's density from LightParams, docs/re/enhanced-lighting.md 5b) -
    // the fog colour's light taken in from the camera out, thinning with
    // height as the haze below. Not on the sky, whose dome carries its own
    // fog band.
    float airT = 1.0;
    vec3 airLight = vec3(0.0);
    if (pc.air.a > 0.0 && d < 1.0) {
        float baseA = pc.air.a * exp(-(cam.z - h0) / kHeightScale);
        float kA = v.z / kHeightScale;
        float odA = abs(kA) < 1e-5 ? baseA * len : baseA * (1.0 - exp(-kA * len)) / kA;
        airT = mix(1.0, exp(-odA * strength), outdoors);
        airLight = fogColor.rgb * (1.0 - airT);
    }

    float run = max(L - kStart, 0.0);
    if (run <= 0.0) {
        if (airT >= 0.999) discard;
        outColor = vec4(airLight, airT);
        return;
    }
    // The ramp: the first kRamp yards past the start count at a growing
    // share, as if the start were soft (half of them, in all).
    run = run > kRamp ? run - 0.5 * kRamp : 0.5 * run * run / kRamp;

    float base = kDensity * exp(-(cam.z + v.z * kStart - h0) / kHeightScale);
    float k = v.z / kHeightScale;
    float depthOD = abs(k) < 1e-5 ? base * run : base * (1.0 - exp(-k * run)) / k;
    float T = exp(-depthOD * strength);

    vec3 travel = normalize(lightDir.xyz);
    float sunUp = smoothstep(0.0, 0.08, -travel.z);
    // Its forward share only toward a sun the sky shows: under a sky with no
    // sun disc (Outland's, LightingParams::sunDisc) the lobe is left out - the
    // even share stays as it was, so the air elsewhere is no brighter - rather
    // than whiting out the sky round a planet as if it were the sun.
    float phase = 0.4 + 0.6 * phaseHG(dot(-travel, v), kG) * clamp(pc.air.r, 0.0, 1.0);
    vec3 light = fogColor.rgb * kSkyScatter + lightColor.rgb * (phase * kSunScatter * sunUp);
    // Capped near white: past it the haze only blows a bright sky and the
    // land under it out (the Barrens toward an evening sun, round 4).
    // With the HDR scene the tone map rolls it off instead, and the glow
    // toward a low sun can be brighter than white (enhanced_tonemap::kHazeCap).
    // In the 8-bit scene it is held under white instead, keeping its hue:
    // the haze lays light over the scene by 1 - T (an over blend), so with
    // its own light under kMaxLight it can tint a bright sky gold but never
    // take it, or the land under it, to white. 1.3 per channel turned the
    // sky round a high sun into a white sheet (Jered's Goldshire road).
    // Under the HDR scene's tone map it may pass white, but it is held by
    // its brightest channel too: clipped per channel at 3 it went white
    // toward a high sun (Westfall at noon) where it should stay gold.
    // A soft knee, not a hard limit: past 70% of the cap the brightest
    // channel eases onto it (Reinhard), so a gradient round the sun has no
    // edge where the limit starts (a hard min printed a ring there).
    float maxLight = (int(pc.target.w + 0.5) & 8) != 0 ? kMaxLightHdr : kMaxLight;
    float top = max(max(light.r, light.g), max(light.b, 1e-4));
    float knee = 0.7 * maxLight;
    float over = max(top - knee, 0.0);
    float eased = min(top, knee) + over * (maxLight - knee) / (maxLight - knee + over);
    light *= eased / top;
    float a = mix(1.0, T, outdoors);
    // The haze is the farther of the two (it starts kStart out): what
    // reaches the eye is the haze's light through the air, and the air's own.
    float total = a * airT;
    if (total >= 0.999) discard;
    if ((int(pc.target.w + 0.5) & 4) != 0) {
        outColor = vec4(vec3(1.0 - total), 0.0);   // WOWEE_CONTACT_SHADOW_DEBUG: how much air
        return;
    }
    outColor = vec4(light * (1.0 - a) * airT + airLight, total);
}
