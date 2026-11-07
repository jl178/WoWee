#version 450

// Smoother characters (Enhancements::characterFidelity): Phong tessellation
// (Boubekeur and Alexa 2008) of the skinned triangle. Each new point is the
// flat point pulled toward the three corners' tangent planes, so a facetted
// outline - a shoulder, a skull, a calf - curves the way its normals already
// say it does, and nothing is added that the mesh does not describe.
//
// The planes are the weld normals: one normal per position, shared by the
// split copies a UV seam or a hard edge makes, so both sides of a seam move
// alike. A crease corner (copies whose normals disagree) has weight 0 and
// stays where it is. Everything happens after skinning and after the robe
// and cloak bends of character.vert, on their result.
//
// The projection is done in clip space as well as in world space: a point's
// tangent-plane offset is d * n, and clip = VP * world is linear, so the clip
// position is the flat one minus d * (VP * n). No matrices are needed here.

layout(triangles, fractional_odd_spacing, cw) in;

layout(location = 0) in vec3 inFragPos[];
layout(location = 1) in vec3 inNormal[];
layout(location = 2) in vec2 inTexCoord[];
layout(location = 3) in vec3 inTangent[];
layout(location = 4) in vec3 inBitangent[];
layout(location = 5) in vec2 inTexCoord2[];
layout(location = 6) in vec4 inClipPos[];
layout(location = 7) in vec4 inWeldN[];
layout(location = 8) in vec4 inClipN[];

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec3 Tangent;
layout(location = 4) out vec3 Bitangent;
layout(location = 5) out vec2 TexCoord2;

// How far toward the tangent planes: 0 flat, 1 the full projection. 0.75 is
// the paper's; at 0.6 boots and gloves puffed out, and 0.4 keeps the art's
// own shapes while the outline loses its facets.
//
// cw: with Vulkan's upper-left domain origin the generated triangles come
// out wound the other way from GL's, and ccw here turned every face
// gl_FrontFacing-backwards, which character.frag lights from behind.
const float kAlpha = 0.4;

void main() {
    vec3 b = gl_TessCoord;
    vec3 p = b.x * inFragPos[0] + b.y * inFragPos[1] + b.z * inFragPos[2];
    vec4 clip = b.x * inClipPos[0] + b.y * inClipPos[1] + b.z * inClipPos[2];

    vec3 offWorld = vec3(0.0);
    vec4 offClip = vec4(0.0);
    for (int i = 0; i < 3; ++i) {
        float d = dot(p - inFragPos[i], inWeldN[i].xyz) * inWeldN[i].w * b[i];
        offWorld += d * inWeldN[i].xyz;
        offClip += d * inClipN[i];
    }

    FragPos = p - kAlpha * offWorld;
    gl_Position = clip - kAlpha * offClip;
    Normal = b.x * inNormal[0] + b.y * inNormal[1] + b.z * inNormal[2];
    TexCoord = b.x * inTexCoord[0] + b.y * inTexCoord[1] + b.z * inTexCoord[2];
    TexCoord2 = b.x * inTexCoord2[0] + b.y * inTexCoord2[1] + b.z * inTexCoord2[2];
    Tangent = b.x * inTangent[0] + b.y * inTangent[1] + b.z * inTangent[2];
    Bitangent = b.x * inBitangent[0] + b.y * inBitangent[1] + b.z * inBitangent[2];
}
