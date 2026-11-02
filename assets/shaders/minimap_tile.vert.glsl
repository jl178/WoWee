#version 450

// Where the tile image's corners land in the composite, in clip space:
// uv00 and uv10 are image (0,0) and (1,0), uv01 and uv11 are (0,1) and (1,1).
// A terrain tile is a ninth of the composite turned a quarter; a WMO tile is
// wherever the building's placement puts it, at any yaw.
layout(push_constant) uniform Push {
    vec4 top;     // uv00.xy, uv10.xy
    vec4 bottom;  // uv01.xy, uv11.xy
} push;

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aUV;
    vec2 top = mix(push.top.xy, push.top.zw, aUV.x);
    vec2 bottom = mix(push.bottom.xy, push.bottom.zw, aUV.x);
    gl_Position = vec4(mix(top, bottom, aUV.y), 0.0, 1.0);
}
