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

    // push.rotation is the player's facing (0 north, a quarter turn west) when
    // the map turns with it - rotateMinimap - and 0 when it is north-up. A
    // screen offset (right, down) is turned back by the facing to a world
    // offset, then read from the composite, whose u runs south and v east.
    // The inverse of ui::minimap_stock::mapOffset, which places the blips.
    float cs = cos(push.rotation);
    float sn = sin(push.rotation);
    vec2 mapUV = push.playerUV +
        vec2(-center.x * sn + center.y * cs, center.x * cs + center.y * sn) * push.zoomRadius * 2.0;

    vec4 mapColor = texture(uComposite, mapUV);

    // No player arrow here: the marker pass draws the stock MinimapArrow
    // texture. push.arrowRotation is left
    // in the block unread.
    //
    // No darkened rim either. Textures\MinimapMask, the stock mask, is a plain
    // disc - 255 alpha to its last texel, nothing shaded - and the border art
    // drawn over the map is what frames it.

    // Always opaque. The stock map is solid whatever the interface's opacity;
    // push.opacity is kept in the block (its layout is shared with the C++ side)
    // but no longer thins the terrain image out.
    outColor = vec4(mapColor.rgb, 1.0);
}
