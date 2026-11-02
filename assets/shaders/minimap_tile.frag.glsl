#version 450

layout(set = 0, binding = 0) uniform sampler2D uTileTexture;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// Sampled as is: the corners in the push constant carry any turn. The alpha
// matters to WMO tiles, which are clear where the group has nothing.
void main() {
    outColor = texture(uTileTexture, TexCoord);
}
