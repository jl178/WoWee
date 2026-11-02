#version 450

// M2 emitter particle quads (m2_particle_gpu.vert). Texture times the
// particle's colour, with the alpha test and fog of the emitter's stock
// blend state (CM2Model's blend table 0xA453B0, alpha reference from
// 0x81FE90; docs/re/particles.md).

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
};

layout(push_constant) uniform Push {
    vec2 tileCount;
    uint blendType;  // the emitter's M2 blend, 0..6
    uint pad;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) in float vFogVisibility;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTexture, vUV) * vColor;
    uint blend = push.blendType;
    float fog = vFogVisibility;
    if (blend == 0u) {
        // Opaque: no test, depth written.
        outColor = vec4(mix(fogColor.rgb, c.rgb, fog), 1.0);
    } else if (blend == 1u) {
        // Alpha key: the reference is 224/255 of the alpha.
        if (c.a < 224.0 / 255.0) discard;
        outColor = vec4(mix(fogColor.rgb, c.rgb, fog), 1.0);
    } else {
        if (c.a < 1.0 / 255.0) discard;
        if (blend == 2u) {
            outColor = vec4(mix(fogColor.rgb, c.rgb, fog), c.a);
        } else if (blend == 3u) {
            // One + One: alpha does not take part. Fog fades it to black.
            outColor = vec4(c.rgb * fog, 1.0);
        } else if (blend == 4u) {
            outColor = vec4(c.rgb * fog, c.a);
        } else if (blend == 5u) {
            // Modulate: fog fades it to white, which changes nothing.
            outColor = vec4(mix(vec3(1.0), c.rgb, fog), c.a);
        } else {
            // Modulate 2x: grey is the neutral colour.
            outColor = vec4(mix(vec3(0.5), c.rgb, fog), c.a);
        }
    }
}
