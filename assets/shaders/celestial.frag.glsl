#version 450

layout(set = 1, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform Push {
    vec4 dirAndSize;   // xyz = unit direction to the sprite, w = half size (radians)
    vec4 color;        // rgb = tint, a = opacity
    vec4 mode;         // x = 1 alpha-blended, 0 additive; y = 1 clip and fade at the horizon
} push;

layout(location = 0) in vec2 TexCoord;
layout(location = 1) in float vHeight;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uSprite, TexCoord);
    float a = tex.a * push.color.a;
    // 0x7edee0: nothing below the eye's height, and the last 0.4 yards of
    // the twelve-yard quad above it fade in.
    if (push.mode.y > 0.5) a *= clamp(vHeight * 2.5, 0.0, 1.0);
    vec3 rgb = tex.rgb * push.color.rgb * a;
    // Premultiplied. The stock sprites go onto the cleared frame and the dome
    // is added over them, which is the same as adding them to the dome:
    // every sky sprite is drawn additive (alpha 0).
    outColor = vec4(rgb, push.mode.x > 0.5 ? a : 0.0);
}
