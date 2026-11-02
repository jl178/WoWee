#version 450

// M2 ribbon emitter fragment shader.
// Samples the ribbon texture, multiplied by vertex color and alpha.
// The blend itself is the pipeline's (alpha, or additive for blends 3 and 4).

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

layout(set = 1, binding = 0) uniform sampler2D uTexture;

// The ribbon's material (M2Material of the model's material table).
layout(push_constant) uniform Push {
    uint blendMode;  // M2 blend, 0..6
    uint unfogged;   // material flag 0x2
} push;

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec2 vUV;
layout(location = 3) in float vFogFactor;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uTexture, vUV);
    // For additive ribbons alpha comes from texture luminance; multiply by vertex alpha.
    float a = tex.a * vAlpha;
    if (a < 0.01) discard;
    vec3 rgb = tex.rgb * vColor;
    // The stock ribbon element (0x820F40) sets its fog up as an M2 batch
    // does (0x81FB10): none with the material's unfogged flag, otherwise a
    // colour by blend (table 0xA45390) - the scene's for opaque, alpha key
    // and alpha, black for the two adds, white for Mod, 0x808080 for Mod2x.
    if (push.unfogged == 0u) {
        vec3 fogTo = fogColor.rgb;
        if (push.blendMode == 3u || push.blendMode == 4u) fogTo = vec3(0.0);
        else if (push.blendMode == 5u) fogTo = vec3(1.0);
        else if (push.blendMode == 6u) fogTo = vec3(128.0 / 255.0);
        rgb = mix(fogTo, rgb, vFogFactor);
    }
    outColor = vec4(rgb, a);
}
