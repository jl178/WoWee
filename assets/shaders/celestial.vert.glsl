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
    vec4 mode;         // x = 1 alpha-blended, 0 additive
} push;

layout(location = 0) out vec2 TexCoord;

// Two triangles, corners from the vertex index - no vertex buffer.
const vec2 kCorners[6] = vec2[](
    vec2(-1.0, -1.0), vec2( 1.0, -1.0), vec2( 1.0,  1.0),
    vec2(-1.0, -1.0), vec2( 1.0,  1.0), vec2(-1.0,  1.0));

void main() {
    vec2 corner = kCorners[gl_VertexIndex];
    TexCoord = corner * vec2(0.5, -0.5) + 0.5;

    // A sprite at infinity facing the eye: its centre in view space by the
    // view's rotation alone, the corners spread square to the line of sight
    // with the view's up kept up, so it is the same angular size anywhere on
    // screen.
    vec3 centre = mat3(view) * normalize(push.dirAndSize.xyz);
    vec3 right = cross(centre, vec3(0.0, 1.0, 0.0));
    right = dot(right, right) > 1e-6 ? normalize(right) : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(right, centre);
    float r = tan(push.dirAndSize.w);
    vec3 pos = centre + (right * corner.x + up * corner.y) * r;
    vec4 clip = projection * vec4(pos, 1.0);
    // On the far plane, so the depth test keeps it behind the world.
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
