#version 450

// The stock selection circle: Textures\UnitSelectTexture.blp projected straight
// down onto the floor around the target. The vertices are a lattice of floor
// samples in world space; the texture coordinates are the stock planar texgen
// over the box feet +/- R, turned so the texture's lower edge faces the camera.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 color;
    vec4 center;  // feet xyz, box half-width R
    vec4 facing;  // camera->unit direction xy, surface lift, box half-height (0: R)
} push;

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec2 vUV;
layout(location = 1) out float vHeight;

void main() {
    float r = push.center.w;
    vec2 d = aPos.xy - push.center.xy;
    vec2 fwd = push.facing.xy;
    vec2 side = vec2(-fwd.y, fwd.x);
    // v grows toward the camera: the near side samples the texture's bright
    // bottom lip.
    vUV = vec2(0.5 + dot(d, side) / (2.0 * r), 0.5 - dot(d, fwd) / (2.0 * r));
    // 0 at the bottom of the box, 1 at its top: the stock height ramp's input.
    // The selection circle's box is as tall as it is wide; the spell
    // targeting circle's is +/- 2 yards whatever its width (0x4F8A40).
    float zr = push.facing.w > 0.0 ? push.facing.w : r;
    vHeight = 0.5 + (aPos.z - push.center.z) / (2.0 * zr);
    gl_Position = push.viewProj * vec4(aPos + vec3(0.0, 0.0, push.facing.z), 1.0);
}
