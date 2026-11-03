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

// specular: rgb the sun colour (DayNight channel 9, the stock c27), w the
// power (20); w = 0 when the `specular` option is off.
layout(push_constant) uniform Push {
    mat4 model;
    vec4 specular;
} push;

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aLayerUV;
layout(location = 4) in vec3 aVertColor;   // MCCV, doubled: 1.0 is neutral

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec2 LayerUV;
layout(location = 4) out vec3 VertColor;
layout(location = 5) out vec3 Specular;

void main() {
    vec4 worldPos = push.model * vec4(aPosition, 1.0);
    FragPos = worldPos.xyz;
    Normal = aNormal;
    TexCoord = aTexCoord;
    LayerUV = aLayerUV;
    VertColor = aVertColor;
    // The stock highlight is per vertex (shaders\vertex\arbvp1\terrain.bls,
    // the specular variants): pow(max(N.H, 0), c27.w) * c27.rgb, H halfway
    // between the eye and the light. The pixel program masks it with the
    // `_s` texture's alpha and the shadow.
    Specular = vec3(0.0);
    if (push.specular.w > 0.0) {
        vec3 toLight = normalize(-lightDir.xyz);
        vec3 toEye = normalize(viewPos.xyz - worldPos.xyz);
        vec3 h = normalize(toLight + toEye);
        Specular = pow(max(dot(normalize(aNormal), h), 0.0), push.specular.w) * push.specular.rgb;
    }
    gl_Position = projection * view * worldPos;
}
