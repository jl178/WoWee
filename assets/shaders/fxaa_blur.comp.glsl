#version 450

// The final pass's quarter-size blurs, worked out once per quarter-size
// texel corner instead of at every pixel (fxaa.frag: stockBlur, bloomAt).
//
// The stock glow's blur (FFXGauss4 across then down) takes sixteen bilinear
// taps of the quarter-size scene at -2.5, -0.5, +0.5 and +2.5 texels from the
// pixel. Each tap is linear in the pixel's position between texel centres,
// and its offset is a half texel, so the sum is bilinear inside every cell
// whose corners are texel *corners* - the points where all sixteen taps land
// on texel centres. Its values there, interpolated by the sampler, are the
// blur itself: the final pass reads one bilinear tap of this image where it
// took sixteen. Here each corner's taps are plain texel reads.
//
// Alpha holds the brightest channel of the 8 x 8 quarter-size texels round
// the corner: the HDR bloom only takes light above its floor (1.6), and a
// pixel whose bloom taps can only reach texels under it adds nothing - which
// is nearly every pixel - so the final pass skips its nine taps there.
//
// One invocation per corner: (quarter width + 1) x (quarter height + 1).

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D uGlow;   // mip 1: the quarter-size scene
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D uOut;

// The corner's texels: from 4 before it to 3 after, on each axis.
const int kLo = 4;
const int kHi = 3;
shared vec3 tile[8 + kLo + kHi][8 + kLo + kHi];

void main() {
    ivec2 size = textureSize(uGlow, 1);
    ivec2 base = ivec2(gl_WorkGroupID.xy) * 8 - kLo;
    const int span = 8 + kLo + kHi;
    // The workgroup's texels, clamped to the image as the sampler's edge
    // mode clamps them.
    for (int i = int(gl_LocalInvocationIndex); i < span * span; i += 64) {
        ivec2 t = ivec2(i % span, i / span);
        ivec2 p = clamp(base + t, ivec2(0), size - 1);
        tile[t.y][t.x] = texelFetch(uGlow, p, 1).rgb;
    }
    barrier();

    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    if (g.x > size.x || g.y > size.y) return;
    ivec2 l = ivec2(gl_LocalInvocationID.xy) + kLo;   // the corner's texel g, in the tile

    // The glow: offsets -2.5, -0.5, +0.5, +2.5 from corner g are texels
    // g - 3, g - 1, g and g + 2; weights 1/8, 3/8, 3/8, 1/8 each way.
    const int offs[4] = int[](-3, -1, 0, 2);
    const float wts[4] = float[](0.125, 0.375, 0.375, 0.125);
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            sum += wts[x] * wts[y] * tile[l.y + offs[y]][l.x + offs[x]];
        }
    }

    float peak = 0.0;
    for (int y = -kLo; y <= kHi; ++y) {
        for (int x = -kLo; x <= kHi; ++x) {
            vec3 c = tile[l.y + y][l.x + x];
            peak = max(peak, max(c.r, max(c.g, c.b)));
        }
    }
    imageStore(uOut, g, vec4(sum, peak));
}
