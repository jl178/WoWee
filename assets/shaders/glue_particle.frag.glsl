#version 450

// Glue scene particles; see glue_particle.vert.glsl. Texture times colour,
// with the stock alpha test: 224/255 for alpha key, 1/255 otherwise.

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    layout(offset = 96) vec2 unused;
    int alphaKey;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTexture, vUV) * vColor;
    if (c.a < (push.alphaKey != 0 ? 224.0 / 255.0 : 1.0 / 255.0)) discard;
    outColor = c;
}
