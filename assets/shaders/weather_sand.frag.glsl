#version 450

// The stock sand: untextured, the fog colour times the grain's fade,
// alpha-blended (sand.bls, 0x78bee0).
layout(push_constant) uniform Push {
    vec4 colour;
    vec4 params;
} push;

layout(location = 0) in float vAlpha;
layout(location = 0) out vec4 outColor;

void main() {
    float a = push.colour.a * vAlpha;
    if (a < 0.004) discard;
    outColor = vec4(push.colour.rgb, a);
}
