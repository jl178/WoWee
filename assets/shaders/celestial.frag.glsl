#version 450

layout(set = 1, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform Push {
    vec4 dirAndSize;   // xyz = unit direction to the sprite, w = half size (radians)
    vec4 color;        // rgb = tint, a = opacity
    vec4 mode;         // x = 1 alpha-blended, 0 additive
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uSprite, TexCoord);
    float a = tex.a * push.color.a;
    vec3 rgb = tex.rgb * push.color.rgb * a;
    // Premultiplied: the moon covers what is behind it, the sun and the
    // glares add to it (alpha 0 leaves the destination at full weight).
    outColor = vec4(rgb, push.mode.x > 0.5 ? a : 0.0);
}
