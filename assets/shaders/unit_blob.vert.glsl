#version 450

// The stock unit blob shadow (docs/re/characters.md section 6, "Unit blob
// shadow: full rule"). The vertices are a lattice of floor samples in world
// space around the unit; the fragment stage does the projection.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 feet;   // xyz the model origin, w = h (half the scaled box height)
    vec4 axes;   // xy the facing, z half-width along it, w half-width across
    vec4 extra;  // x model alpha, y surface lift
} push;

layout(location = 0) in vec3 aPos;
layout(location = 0) out vec3 vWorld;

void main() {
    vWorld = aPos;
    gl_Position = push.viewProj * vec4(aPos + vec3(0.0, 0.0, push.extra.y), 1.0);
}
