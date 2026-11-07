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

// The stock sand (sand.bls, MapWeather 0x78bee0): every grain is moved here
// from where and when it was born, as the stock vertex program moves its
// packets. c9 = (-0.05, 1.5, viewport width * 0.0025): a grain is
// clamp(1.5 - 0.05 d) of 0.0025 of the width in pixels across - full to
// 10 yd, gone at 30 - and fades in and out over 0.2 s.
layout(push_constant) uniform Push {
    vec4 colour;   // the fog colour (c0), alpha 1
    vec4 params;   // x: the weather clock, y: half a grain's side per yard of distance
} push;

layout(location = 0) in vec3 aOrigin;
layout(location = 1) in vec3 aVelocity;
layout(location = 2) in vec2 aTimes;   // birth, life

layout(location = 0) out float vAlpha;

const vec2 kCorners[6] = vec2[6](vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(1.0, -1.0),
                                 vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(-1.0, -1.0));

void main() {
    float age = push.params.x - aTimes.x;
    vec3 pos = aOrigin + aVelocity * clamp(age, 0.0, aTimes.y);
    float d = length(pos - viewPos.xyz);
    float size = clamp(1.5 - 0.05 * d, 0.0, 1.0);
    if (age < 0.0 || age > aTimes.y || size <= 0.0 || d < 0.3) {
        vAlpha = 0.0;
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // outside the clip volume
        return;
    }
    vAlpha = clamp(5.0 * age, 0.0, 1.0) * clamp(5.0 * (aTimes.y - age), 0.0, 1.0);
    vec3 right = vec3(view[0][0], view[1][0], view[2][0]);
    vec3 up = vec3(view[0][1], view[1][1], view[2][1]);
    float halfSide = size * push.params.y * d;
    vec2 c = kCorners[gl_VertexIndex % 6];
    pos += (right * c.x + up * c.y) * halfSide;
    gl_Position = projection * view * vec4(pos, 1.0);
}
