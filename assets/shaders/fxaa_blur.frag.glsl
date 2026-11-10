#version 450

// The final pass's quarter-size blurs, worked out once per quarter-size
// texel corner instead of at every pixel (fxaa.frag: stockBlur, bloomAt).
// One fragment per corner: the target is (quarter width + 1) x (quarter
// height + 1), and gl_FragCoord's whole part is the corner's index.
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
// A fragment pass rather than a compute dispatch: on the Iris Xe the switch
// to the compute pipeline and back cost more than the work.

layout(set = 0, binding = 0) uniform sampler2D uGlow;   // mip 1: the quarter-size scene

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 size = textureSize(uGlow, 1);
    ivec2 g = ivec2(gl_FragCoord.xy);
    // The glow: offsets -2.5, -0.5, +0.5, +2.5 from corner g are texels
    // g - 3, g - 1, g and g + 2; weights 1/8, 3/8, 3/8, 1/8 each way. The
    // peak: texels g - 4 to g + 3. One read of each, clamped to the image as
    // the sampler's edge mode clamps.
    const float wts[8] = float[](0.0, 0.125, 0.0, 0.375, 0.375, 0.0, 0.125, 0.0);
    vec3 sum = vec3(0.0);
    float peak = 0.0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            ivec2 p = clamp(g + ivec2(x, y) - 4, ivec2(0), size - 1);
            vec3 c = texelFetch(uGlow, p, 1).rgb;
            float w = wts[x] * wts[y];
            if (w > 0.0) sum += w * c;   // (unrolled: the zero taps vanish)
            peak = max(peak, max(c.r, max(c.g, c.b)));
        }
    }
    outColor = vec4(sum, peak);
}
