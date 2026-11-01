#version 450

// Glue scene particles; see glue_particle.vert.glsl. Same look as
// m2_particle.frag.glsl, without the fog.

layout(set = 0, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    layout(offset = 96) vec2 tileCount;
    int alphaKey;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 p = gl_PointCoord;
    vec2 tiles = max(push.tileCount, vec2(1.0));
    float tile = floor(vTile);
    float tx = mod(tile, tiles.x);
    float ty = floor(tile / tiles.x);
    vec2 uv = (vec2(tx, ty) + p) / tiles;
    vec4 texColor = texture(uTexture, uv);

    if (push.alphaKey != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        if (lum < 0.05) discard;
    }

    float edge = 1.0 - smoothstep(0.4, 0.5, length(p - 0.5));
    float alpha = texColor.a * vColor.a * edge;
    outColor = vec4(texColor.rgb * vColor.rgb * alpha, alpha);
}
