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
};

layout(set = 1, binding = 0) uniform sampler2D uSprite;

layout(push_constant) uniform Push {
    vec4 tint;   // rgb = colour, a = opacity
} push;

layout(location = 0) in vec2 vUV;
layout(location = 1) in float vAlpha;
layout(location = 2) in float vFog;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uSprite, vUV);
    if (push.tint.a < 0.0) {
        // Rain and splashes, the stock GxBlend_Mod2x: the framebuffer is
        // multiplied by twice this. The textures sit at grey 128, so a drop
        // barely brightens what is behind it; high above the eye (the stock
        // rain fog, weather.vert), and toward the edges of the box this keeps
        // around the camera (Wowee's wrap fade), it goes to the neutral grey
        // 0x80 (the stock fog colour 0x80808080 for this blend). Alpha only
        // cuts the sprite out (alpha test ref 1, 0xad8b7c).
        if (tex.a < 1.0 / 255.0) discard;
        vec3 c = mix(vec3(128.0 / 255.0), tex.rgb * push.tint.rgb, clamp(vFog * vAlpha, 0.0, 1.0));
        outColor = vec4(c, 1.0);
        return;
    }
    // Snow: the Weather.dbc colour over the flake, alpha-blended, unlit and
    // unfogged as snowpoint.bls draws it.
    float alpha = tex.a * vAlpha * push.tint.a;
    if (alpha < 0.004) discard;
    outColor = vec4(tex.rgb * push.tint.rgb, alpha);
}
