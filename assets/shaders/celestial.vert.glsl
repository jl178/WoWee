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

layout(push_constant) uniform Push {
    vec4 dirAndSize;   // xyz = unit direction to the sprite, w = half size (radians)
    vec4 color;        // rgb = tint, a = opacity
    vec4 mode;         // x = 1 alpha-blended, 0 additive; y = 1 clip and fade at the horizon
} push;

layout(location = 0) out vec2 TexCoord;
// The corner's height above the eye, in yards, on the stock quad twelve yards
// out (0xa1047c) - what 0x7edee0 clips and fades by.
layout(location = 1) out float vHeight;

// Two triangles, corners from the vertex index - no vertex buffer.
const vec2 kCorners[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));

void main() {
    vec2 corner = kCorners[gl_VertexIndex];
    TexCoord = corner * vec2(0.5, -0.5) + 0.5;

    // The stock billboard (0x9abb60): facing the eye, its sides kept level
    // with the world's horizon rather than the view's.
    vec3 dir = normalize(push.dirAndSize.xyz);
    vec3 right = vec3(-dir.y, dir.x, 0.0);
    right = dot(right, right) > 1e-8 ? normalize(right) : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(dir, right);
    float r = tan(push.dirAndSize.w);
    vec3 worldPos = dir + (right * corner.x + up * corner.y) * r;
    vHeight = worldPos.z * 12.0;
    vec4 clip = projection * vec4(mat3(view) * worldPos, 1.0);
    // On the far plane, so the depth test keeps it behind the world.
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
