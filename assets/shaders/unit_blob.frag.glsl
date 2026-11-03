#version 450

// The stock unit blob shadow (Wow.exe 0x7E4480 / 0x7E4370, docs/re/characters.md
// section 6): ShadowBlob.blp in model axes over the footprint, stage 0 Fade
// (lerp(white, blob, A)), stage 1 Add with the "ShadowMod" 64x8 ramp indexed by
// height through the box, the result multiplied into the floor (GxBlend Mod):
//     dst' = dst * min(1, lerp(1, blob.rgb, A) + (1 - r(12 u1)))
// r rises over the bottom sixth of the box, is 1, and falls over the top sixth.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 feet;
    vec4 axes;
    vec4 extra;
} push;

layout(set = 0, binding = 0) uniform sampler2D uBlob;

layout(location = 0) in vec3 vWorld;
layout(location = 0) out vec4 outColor;

float ramp(float x) {
    if (x < 2.0) return x * 0.5;
    if (x < 10.0) return 1.0;
    return max(0.0, (12.0 - x) * 0.5);
}

void main() {
    vec2 d = vWorld.xy - push.feet.xy;
    vec2 fwd = push.axes.xy;
    vec2 side = vec2(-fwd.y, fwd.x);
    float lx = dot(d, fwd);
    float ly = dot(d, side);
    vec2 uv = vec2(0.5 + ly / (2.0 * push.axes.w), 0.5 - lx / (2.0 * push.axes.z));
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) discard;

    float h = push.feet.w;
    float zMin = push.feet.z - 1.6667 * h;
    float zMax = push.feet.z + h;
    float u1 = (vWorld.z - zMin) / max(zMax - zMin, 1e-4);
    // The ramp texture is 64 texels sampled linearly with its edge clamped;
    // texel i holds r(12 i / 63).
    float ti = clamp(u1 * 64.0 - 0.5, 0.0, 63.0);
    float i0 = floor(ti);
    float r = mix(ramp(12.0 * i0 / 63.0), ramp(12.0 * min(i0 + 1.0, 63.0) / 63.0), ti - i0);
    if (r <= 0.0) discard;

    float a = floor(clamp(push.extra.x, 0.0, 1.0) * 255.0 + 0.5) / 255.0;
    vec3 blob = texture(uBlob, uv).rgb;
    vec3 c = min(vec3(1.0), mix(vec3(1.0), blob, a) + vec3(1.0 - r));
    outColor = vec4(c, 1.0);
}
