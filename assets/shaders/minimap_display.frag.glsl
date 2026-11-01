#version 450

layout(set = 0, binding = 0) uniform sampler2D uComposite;

layout(push_constant) uniform Push {
    vec4 rect;
    vec2 playerUV;
    float rotation;
    float arrowRotation;
    float zoomRadius;
    int squareShape;
    float opacity;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

float cross2d(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

bool pointInTriangle(vec2 p, vec2 a, vec2 b, vec2 c) {
    float d1 = cross2d(b - a, p - a);
    float d2 = cross2d(c - b, p - b);
    float d3 = cross2d(a - c, p - c);
    bool hasNeg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
    bool hasPos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
    return !(hasNeg && hasPos);
}

void main() {
    vec2 center = TexCoord - 0.5;
    float dist = length(center);

    if (push.squareShape == 0) {
        if (dist > 0.5) discard;
    }

    float cs = cos(push.rotation);
    float sn = sin(push.rotation);
    vec2 mapCenter = vec2(-center.x, center.y);
    vec2 rotated = vec2(mapCenter.x * cs - mapCenter.y * sn, mapCenter.x * sn + mapCenter.y * cs);
    vec2 mapUV = push.playerUV + vec2(rotated.y, -rotated.x) * push.zoomRadius * 2.0;

    vec4 mapColor = texture(uComposite, mapUV);

    // No player arrow here. It used to be a yellow triangle and a white dot
    // drawn into the map, under the textured MinimapArrow.blp the marker pass
    // draws - two arrows at the centre. The stock client has the one, so the
    // marker pass owns it; push.arrowRotation is left in the block unread.

    // Dark border ring
    float border = smoothstep(0.48, 0.5, dist);
    if (push.squareShape == 0) {
        mapColor.rgb *= 1.0 - border * 0.7;
    }

    // Always opaque. The stock map is solid whatever the interface's opacity;
    // push.opacity is kept in the block (its layout is shared with the C++ side)
    // but no longer thins the terrain image out.
    outColor = vec4(mapColor.rgb, 1.0);
}
