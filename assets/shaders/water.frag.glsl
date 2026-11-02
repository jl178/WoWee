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
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    mat4 model;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType;
    vec2 screenSize;  // size of the target being drawn into
    vec2 depthRange;  // the camera's own near and far, for linearising SceneDepth
    float sceneValid; // 1 once the depth capture holds a frame, 0 before it
    float liquidTexValid; // 1 when set 3 holds this liquid's texture frame
    float shoreFoam;      // 1 draws the shoreline foam band (not stock), 0 off
    float pad2;
} push;

layout(set = 1, binding = 0) uniform WaterMaterial {
    vec4 waterColor;       // unused by the stock path: colours come from Light.dbc
    float waterAlpha;      // unused by the stock path: alpha comes from LightParams
    float shimmerStrength; // sun glint strength
    float alphaScale;
    float bodyFloor;       // unused by the stock path
};

// SceneColor is still bound but no longer read: the stock water does not
// refract. Only the depth copy is used, for the water's depth.
layout(set = 2, binding = 0) uniform sampler2D SceneColor;
layout(set = 2, binding = 1) uniform sampler2D SceneDepth;
layout(set = 2, binding = 2) uniform sampler2D ReflectionColor;
// Wake points are laid down behind anything wading through the shallows. Each
// is a churn site that spreads and dies, so a running character leaves a trail
// of froth rather than a disc stuck under its feet.
const int MAX_WAKE_POINTS = 32;

layout(set = 2, binding = 3) uniform WaterFrameData {
    mat4 reflViewProj;
    vec4 wakeBounds;                  // xy = centre, z = cull radius, w = count
    vec4 wakePoints[MAX_WAKE_POINTS]; // xy = world pos, z = age 0..1, w = strength
    // Light.dbc liquid colours (LightIntBand 14-17), rgb, with LightParams'
    // shallow alpha in the close colour's a and deep alpha in the far one's.
    vec4 riverClose;
    vec4 riverFar;
    vec4 oceanClose;
    vec4 oceanFar;
};

// One frame of the liquid's LiquidType texture sequence (lake_a.%d.blp and
// the rest), picked by the renderer from the clock.
layout(set = 3, binding = 0) uniform sampler2D LiquidTexture;

// Yards per repeat of the liquid texture: half an ADT chunk, four liquid cells.
const float LIQUID_TEX_REPEAT = 33.333333 / 2.0;
// Yards of depth over which the colour and alpha run from close to far.
const float RIVER_RAMP_DEPTH = 6.0;
const float OCEAN_RAMP_DEPTH = 15.0;
// The shoreline foam: yards of depth the band reaches out to at most, how
// opaque it gets on the ocean (rivers and lakes take 70% of that), and how
// pale it is against the light falling on it.
const float SHORE_FOAM_DEPTH = 0.5;
const float SHORE_FOAM_OPACITY = 0.45;
const float SHORE_FOAM_ALBEDO = 0.72;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in float WaveOffset;
layout(location = 4) in vec2 ScreenUV;

layout(location = 0) out vec4 outColor;

// ============================================================
// Dual-scroll detail normals (multi-octave ripple overlay)
// ============================================================
float noiseValue(vec2 p);  // defined below; used to bend the wave fronts

vec3 dualScrollWaveNormal(vec2 p, float time) {
    // Three wave octaves at different angles, frequencies, and speeds.
    // Directions are non-axis-aligned to prevent visible tiling patterns.
    // Frequency increases and amplitude decreases per octave (standard
    // multi-octave noise layering for natural water appearance).
    vec2 d1 = normalize(vec2(0.86, 0.51));   // ~30° from +X
    vec2 d2 = normalize(vec2(-0.47, 0.88));  // ~118° (opposing cross-wave)
    vec2 d3 = normalize(vec2(0.32, -0.95));  // ~-71° (third axis for variety)
    float f1 = 0.19, f2 = 0.43, f3 = 0.72;  // spatial frequency (higher = tighter ripples)
    float s1 = 0.95, s2 = 1.73, s3 = 2.40;  // scroll speed (higher octaves move faster)
    float a1 = 0.22, a2 = 0.10, a3 = 0.05;  // amplitude (decreasing for natural falloff)

    vec2 p1 = p + d1 * (time * s1 * 4.0);
    vec2 p2 = p + d2 * (time * s2 * 4.0);
    vec2 p3 = p + d3 * (time * s3 * 4.0);

    // Bend the wave fronts. Each octave on its own is a pure cosine, which is a
    // set of infinitely long straight ridges - in perspective those read as
    // bright parallel lines running to the horizon, and the specular highlight
    // rides along each crest. Perturbing the phase with low-frequency noise
    // makes the crests wander, at the same wavelength, amplitude and speed. The
    // noise is much coarser than the waves, so it curves them rather than
    // roughening them.
    float warp = noiseValue(p * 0.018 + vec2(time * 0.015)) * 6.28318;
    float c1 = cos(dot(p1, d1) * f1 + warp);
    float c2 = cos(dot(p2, d2) * f2 + warp * 1.7);
    float c3 = cos(dot(p3, d3) * f3 + warp * 2.6);

    float dHx = c1 * d1.x * f1 * a1 + c2 * d2.x * f2 * a2 + c3 * d3.x * f3 * a3;
    float dHy = c1 * d1.y * f1 * a1 + c2 * d2.y * f2 * a2 + c3 * d3.y * f3 * a3;

    return normalize(vec3(-dHx, -dHy, 1.0));
}

// ============================================================
// GGX/Cook-Torrance BRDF
// ============================================================
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * denom * denom + 1e-7);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
    return ggx1 * ggx2;
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================
// Linearize depth
// ============================================================
float linearizeDepth(float d, float near, float far) {
    return near * far / (far - d * (far - near));
}

// ============================================================
// Noise functions for foam
// ============================================================
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float hash22x(vec2 p) {
    return fract(sin(dot(p, vec2(269.5, 183.3))) * 43758.5453);
}

float noiseValue(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbmNoise(vec2 p, float time) {
    float v = 0.0;
    v += noiseValue(p * 3.0 + time * 0.3) * 0.5;
    v += noiseValue(p * 6.0 - time * 0.5) * 0.25;
    v += noiseValue(p * 12.0 + time * 0.7) * 0.125;
    return v;
}

// ============================================================
// Gradient (Perlin) noise + rotated fractal - for the magma/slime flow
// ============================================================
// Value noise (noiseValue above) samples a scalar per lattice point and
// interpolates it, so its features sit square on the integer grid and read as
// axis-aligned blocks up close - which is what made the slime motion look like
// tiles. Gradient noise instead interpolates a random gradient that is zero at
// each lattice point, so neighbouring cells blend through zero without the flat
// plateaus, and rotating every octave keeps the separate lattices from ever
// lining up. The result is a fractal flow rather than a grid of squares.
vec2 gradHash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

float gradNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = dot(gradHash(i + vec2(0.0, 0.0)), f - vec2(0.0, 0.0));
    float b = dot(gradHash(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(gradHash(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(gradHash(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.5 + 0.5;
}

// Same octave structure and value range as fbmNoise, but on gradient noise and
// with a small rotation between octaves so no two lattices align.
float fbmGrad(vec2 p, float time) {
    const mat2 R = mat2(0.80, -0.60, 0.60, 0.80);   // ~37° per octave
    float v = 0.0;
    v += gradNoise(p * 3.0 + time * 0.3) * 0.5;   p = R * p;
    v += gradNoise(p * 6.0 - time * 0.5) * 0.25;  p = R * p;
    v += gradNoise(p * 12.0 + time * 0.7) * 0.125;
    return v;
}

// Voronoi-like cellular noise for foam particles
// jitter parameter controls how much cell points deviate from grid centers
// (0.0 = regular grid, 1.0 = fully random within cell)
float cellularFoam(vec2 p, float jitter) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float minDist = 1.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 cellId = i + neighbor;
            // Jittered cell point - higher jitter = more irregular placement
            vec2 point = vec2(hash21(cellId), hash22x(cellId)) * jitter
                       + vec2(0.5) * (1.0 - jitter);
            float d = length(neighbor + point - f);
            minDist = min(minDist, d);
        }
    }
    return minDist;
}
float cellularFoam(vec2 p) { return cellularFoam(p, 1.0); }

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// The zone's distance fog, then the air in front of it. The distance fog is
// the far haze the sky is painted to meet, so it goes on first; the volume is
// everything between the camera and that, sunlit shafts and torch glow
// included.
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    float fogFactor = clamp((fogParams.y - dist) / (fogParams.y - fogParams.x), 0.0, 1.0);
    color = mix(fogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

// The water's depth below this pixel, in yards straight down, or -1 when it
// cannot be known.
//
// The depth copy is half resolution and holds everything drawn before the
// water, the player included. Where a character stands in front of the water,
// the taps along its outline read the character's own depth, nearer than the
// surface. Taken as they were, that read as zero depth: the shallow colour and
// alpha, wobbling with the surface, traced the character's outline over the
// water. A sample nearer than the surface is not a bed under the water, so it
// is not used; the neighbours are tried instead, and only when none of them
// lies behind the surface is the depth unknown.
float waterDepthBelow(vec2 screenUV, float waterLinDepth, float near, float far, float verticalFactor) {
    if (push.sceneValid < 0.5) return -1.0;
    vec2 texel = 1.0 / vec2(textureSize(SceneDepth, 0));
    const vec2 taps[5] = vec2[5](vec2(0.0), vec2(1.5, 0.0), vec2(-1.5, 0.0), vec2(0.0, 1.5), vec2(0.0, -1.5));
    for (int i = 0; i < 5; ++i) {
        vec2 uv = clamp(screenUV + taps[i] * texel, vec2(0.001), vec2(0.999));
        float sceneLin = linearizeDepth(texture(SceneDepth, uv).r, near, far);
        // A small allowance for the depth copy's precision and the surface's
        // own ripple: a bed within a few centimetres of the surface is the
        // waterline, not an occluder.
        if (sceneLin >= waterLinDepth - 0.05) {
            return max(sceneLin - waterLinDepth, 0.0) * max(verticalFactor, 0.05);
        }
    }
    return -1.0;
}

void main() {
    float time = fogParams.z;
    float basicType = push.liquidBasicType;
    float dist = length(viewPos.xyz - FragPos);

    // The LiquidType texture, one frame of its animated sequence, chosen on
    // the CPU and bound as set 3. Tiled in world space so neighbouring chunks
    // and WMO liquids line up.
    bool hasLiquidTex = push.liquidTexValid > 0.5;
    vec4 texel = texture(LiquidTexture, FragPos.xy / LIQUID_TEX_REPEAT);

    // ============================================================
    // Magma / Slime - the texture sequence itself, opaque and unlit
    // ============================================================
    if (basicType > 1.5) {
        vec3 color;
        if (hasLiquidTex) {
            color = texel.rgb;
        } else {
            // No texture to draw: a flowing procedural stand-in.
            vec2 flowUV = FragPos.xy;
            bool isMagma = basicType < 2.5;
            float n1 = fbmGrad(flowUV * 0.06 + vec2(time * 0.02, time * 0.03), time * 0.4);
            float n2 = fbmGrad(flowUV * 0.10 + vec2(-time * 0.015, time * 0.025), time * 0.3);
            float n3 = gradNoise(flowUV * 0.25 + vec2(time * 0.04, -time * 0.02));
            float flow = n1 * 0.45 + n2 * 0.35 + n3 * 0.20;
            vec3 crustColor = isMagma ? vec3(0.15, 0.04, 0.01) : vec3(0.05, 0.15, 0.02);
            vec3 hotColor   = isMagma ? vec3(1.0, 0.45, 0.05) : vec3(0.3, 0.8, 0.15);
            vec3 coreColor  = isMagma ? vec3(1.0, 0.85, 0.3)  : vec3(0.5, 1.0, 0.3);
            float crustMask = smoothstep(0.25, 0.50, flow);
            float coreMask  = smoothstep(0.60, 0.80, flow);
            color = mix(mix(crustColor, hotColor, crustMask), coreColor, coreMask);
        }
        color = applyFog(color, FragPos, dist);
        outColor = vec4(color, 1.0);
        return;
    }

    vec2 screenUV = gl_FragCoord.xy / max(push.screenSize, vec2(1.0));
    vec3 viewDir = normalize(viewPos.xyz - FragPos);
    vec3 ldir = normalize(-lightDir.xyz);

    // ============================================================
    // Depth below the surface
    // ============================================================
    // Only ever sampled straight down the pixel. There is no refraction: the
    // 3.3.5 water does not bend what lies under or behind it, and the offset
    // lookup into the scene copy is what smeared characters standing in front
    // of the water across its surface.
    float near = push.depthRange.x;
    float far = push.depthRange.y;
    float waterLinDepth = linearizeDepth(gl_FragCoord.z, near, far);
    float depthBelow = waterDepthBelow(screenUV, waterLinDepth, near, far, abs(viewDir.z));

    // ============================================================
    // Stock colour and alpha: Light.dbc close -> far by depth
    // ============================================================
    bool isOcean = basicType > 0.5;
    vec4 closeCol = isOcean ? oceanClose : riverClose;   // a = shallow alpha
    vec4 farCol   = isOcean ? oceanFar   : riverFar;     // a = deep alpha
    // Yards over which the ramp runs from the shallow end to the deep end.
    float rampDepth = isOcean ? OCEAN_RAMP_DEPTH : RIVER_RAMP_DEPTH;
    // Unknown depth is open water, the deep end.
    float depth01 = depthBelow < 0.0 ? 1.0 : clamp(depthBelow / rampDepth, 0.0, 1.0);
    vec4 lerpCol = mix(closeCol, farCol, depth01);

    // The texture is a dark ripple layer added over the colour, as the stock
    // liquid shader does; the alpha stays the depth ramp's.
    vec3 color = lerpCol.rgb + (hasLiquidTex ? texel.rgb : vec3(0.0));
    float alpha = lerpCol.a;

    // ============================================================
    // Sun glint
    // ============================================================
    vec3 norm = vec3(0.0, 0.0, 1.0);
    float detailFade = 1.0 - smoothstep(250.0, 1400.0, dist);
    if (detailFade > 0.0) {
        norm = normalize(mix(norm, dualScrollWaveNormal(FragPos.xy, time), 0.45 * detailFade));
    }
    float NdotV = max(dot(norm, viewDir), 0.001);
    float NdotL = max(dot(norm, ldir), 0.0);
    const vec3 F0 = vec3(0.02);
    float fresnel = F0.x + (1.0 - F0.x) * pow(1.0 - NdotV, 5.0);

    float roughness = 0.22;
    vec3 halfDir = normalize(ldir + viewDir);
    float D = DistributionGGX(norm, halfDir, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlickRoughness(max(dot(halfDir, viewDir), 0.0), F0, roughness);
    vec3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.001) * lightColor.rgb * NdotL;
    specular = min(specular * 0.6, vec3(1.0)) * shimmerStrength;

    // ============================================================
    // Sky reflection - only where the reflection pass drew one
    // ============================================================
    // The reflection target holds the sky alone. When the pass does not run
    // it is black, and nothing is mixed in.
    vec4 reflClip = reflViewProj * vec4(FragPos, 1.0);
    if (reflClip.w > 0.1) {
        vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;
        reflUV.y = 1.0 - reflUV.y;
        reflUV += norm.xy * 0.015;
        float edgeFade = smoothstep(0.0, 0.15, reflUV.x) * smoothstep(1.0, 0.85, reflUV.x)
                       * smoothstep(0.0, 0.15, reflUV.y) * smoothstep(1.0, 0.85, reflUV.y);
        vec3 sky = texture(ReflectionColor, clamp(reflUV, vec2(0.002), vec2(0.998))).rgb;
        float valid = smoothstep(0.002, 0.05, dot(sky, vec3(0.299, 0.587, 0.114))) * edgeFade;
        color = mix(color, sky, clamp(fresnel * valid, 0.0, 0.35));
    }

    color += specular;
    // A glint shows over shallow, mostly clear water as well as deep.
    alpha = max(alpha, clamp(dot(specular, vec3(0.333)), 0.0, 1.0));

    // ============================================================
    // Shoreline foam - not stock, the "Shoreline foam" setting
    // ============================================================
    // A thin band where the water runs out over the bed: the same depth below
    // the surface that drives the colour ramp, so it follows every beach,
    // bank, rock and pier post without any authored data. The band's reach
    // breathes slowly, out of step along the shore, and a scrolled noise
    // breaks it into patches. Lit by the sky's ambient and sun as a flat
    // pale surface would be, and fogged with the water below, so it stays a
    // pale edge at dusk and a dim one at night rather than a white line.
    // Two value-noise lookups for the pixels in the band, nothing elsewhere.
    float shoreFoam = 0.0;
    if (push.shoreFoam > 0.5 && depthBelow >= 0.0) {
        float near01 = 1.0 - smoothstep(40.0, 150.0, dist);
        float phase = noiseValue(FragPos.xy * 0.07) * 6.28318;
        float reach = SHORE_FOAM_DEPTH * (0.75 + 0.25 * sin(time * 0.8 + phase));
        float band = 1.0 - smoothstep(0.0, reach, depthBelow);
        band *= band * near01;
        if (band > 0.002) {
            vec2 drift = vec2(time * 0.09, -time * 0.06);
            float n = noiseValue(FragPos.xy * 0.9 + drift) * 0.6
                    + noiseValue(FragPos.xy * 2.3 - drift * 1.7) * 0.4;
            // Solid right at the waterline, broken up a little way out.
            float cover = mix(smoothstep(0.30, 0.70, n), 1.0, band * 0.5);
            shoreFoam = band * cover * (isOcean ? SHORE_FOAM_OPACITY : SHORE_FOAM_OPACITY * 0.7);
            vec3 foamLight = ambientColor.rgb + lightColor.rgb * max(ldir.z, 0.0);
            vec3 foamColor = min(SHORE_FOAM_ALBEDO * foamLight, vec3(1.0));
            color = mix(color, foamColor, shoreFoam);
            alpha = max(alpha, shoreFoam);
        }
    }

    // ============================================================
    // Wake froth - water churned up by something moving through it
    // ============================================================
    float wakeFoam = 0.0;
    if (wakeBounds.w > 0.5) {
        vec2 toWake = FragPos.xy - wakeBounds.xy;
        if (dot(toWake, toWake) < wakeBounds.z * wakeBounds.z) {
            int wakeCount = clamp(int(wakeBounds.w), 0, MAX_WAKE_POINTS);
            for (int i = 0; i < wakeCount; ++i) {
                vec4 wp = wakePoints[i];
                float radius = mix(0.40, 1.45, wp.z);
                float d = length(FragPos.xy - wp.xy);
                float disc = 1.0 - smoothstep(0.0, radius, d);
                wakeFoam += disc * disc * wp.w * (1.0 - wp.z);
            }
            wakeFoam = clamp(wakeFoam, 0.0, 1.0);
            if (wakeFoam > 0.002) {
                vec2 churnWarp = vec2(
                    noiseValue(FragPos.xy * 3.4 + time * 0.5) - 0.5,
                    noiseValue(FragPos.xy * 3.4 + vec2(23.0) - time * 0.44) - 0.5
                ) * 0.9;
                vec2 churnUV = FragPos.xy + churnWarp;
                float cells = cellularFoam(churnUV * 12.0 + time * vec2(0.25, -0.18));
                float cover = 1.0 - smoothstep(0.08, 0.38, cells);
                float fine = cellularFoam(churnUV * 29.0 - time * vec2(0.14, 0.30));
                cover = max(cover * 0.85, (1.0 - smoothstep(0.05, 0.22, fine)) * 0.5);
                cover *= 0.55 + 0.45 * noiseValue(churnUV * 2.2 + time * 0.35);
                wakeFoam = min(wakeFoam * cover, 0.60);
                color = mix(color, vec3(0.86, 0.91, 0.95), wakeFoam);
            }
        }
    }
    alpha = max(alpha, wakeFoam * 0.55);

    alpha *= alphaScale;
    alpha = clamp(alpha, 0.0, 1.0);
    // Dissolve the sheet before the water geometry runs out, so the ocean fades
    // into the horizon haze instead of ending on a hard line.
    alpha *= smoothstep(2400.0, 600.0, dist);

    color = applyFog(color, FragPos, dist);
    outColor = vec4(color, alpha);
}
